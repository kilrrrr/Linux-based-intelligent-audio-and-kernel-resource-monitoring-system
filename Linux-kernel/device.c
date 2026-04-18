#include "device.h"

/*
 * These defaults are only a bring-up placeholder.
 * Replace them with verified target-board GPIOs after hardware confirmation.
 */
static int button_gpios[BUTTON_COUNT] = {
	17, /* KEY1 */
	18, /* KEY2 */
	27, /* KEY3 */
	22, /* KEY4 */
	23, /* KEY5 */
	24, /* KEY6 */
};
static int button_gpio_count = BUTTON_COUNT;
module_param_array(button_gpios, int, &button_gpio_count, 0444);
MODULE_PARM_DESC(button_gpios, "GPIO list for 6 buttons");

static int led_gpios[LED_COUNT] = {
	5,  /* LED0 */
	6,  /* LED1 */
	13, /* LED2 */
	19, /* LED3 */
};
static int led_gpio_count = LED_COUNT;
module_param_array(led_gpios, int, &led_gpio_count, 0444);
MODULE_PARM_DESC(led_gpios, "GPIO list for 4 leds");

static bool button_active_low = true;
module_param(button_active_low, bool, 0444);
MODULE_PARM_DESC(button_active_low, "Buttons are active low when true");

static bool led_active_high = true;
module_param(led_active_high, bool, 0444);
MODULE_PARM_DESC(led_active_high, "Leds are active high when true");

struct music_gpio_drv g_music_drv;

static void music_update_button_state(bool mark_change)
{
	unsigned char new_state[BUTTON_COUNT];
	unsigned long flags;
	bool changed = false;
	int i;

	for (i = 0; i < BUTTON_COUNT; ++i)
	{
		int value = gpio_get_value(g_music_drv.button_gpios[i]);
		bool pressed = g_music_drv.button_active_low ? (value == 0) : (value != 0);

		new_state[i] = pressed ? BUTTON_PRESSED_CHAR : BUTTON_RELEASED_CHAR;
	}

	spin_lock_irqsave(&g_music_drv.button_lock, flags);
	for (i = 0; i < BUTTON_COUNT; ++i)
	{
		if (g_music_drv.button_state[i] != new_state[i])
		{
			changed = true;
			break;
		}
	}

	memcpy(g_music_drv.button_state, new_state, sizeof(new_state));
	if (mark_change && changed)
	{
		g_music_drv.buttons_changed = true;
	}
	spin_unlock_irqrestore(&g_music_drv.button_lock, flags);

	if (mark_change && changed)
	{
		wake_up_interruptible(&g_music_drv.button_waitq);
	}
}

int music_set_led(unsigned int which, bool on)
{
	int value;

	if (which >= LED_COUNT)
	{
		return -EINVAL;
	}

	value = on ? 1 : 0;
	if (!g_music_drv.led_active_high)
	{
		value = !value;
	}

	mutex_lock(&g_music_drv.led_lock);
	gpio_set_value(g_music_drv.led_gpios[which], value);
	mutex_unlock(&g_music_drv.led_lock);

	return 0;
}

void music_button_debounce_timer(struct timer_list *t)
{
	(void)t;
	music_update_button_state(true);
}

irqreturn_t music_button_irq_handler(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;

	mod_timer(&g_music_drv.debounce_timer, jiffies + msecs_to_jiffies(DEBOUNCE_MS));
	return IRQ_HANDLED;
}

static void music_release_leds(int count)
{
	while (--count >= 0)
	{
		gpio_set_value(g_music_drv.led_gpios[count], g_music_drv.led_active_high ? 0 : 1);
		gpio_free(g_music_drv.led_gpios[count]);
	}
}

static void music_release_buttons(int count)
{
	while (--count >= 0)
	{
		if (g_music_drv.button_irqs[count] >= 0)
		{
			free_irq(g_music_drv.button_irqs[count], &g_music_drv);
		}
		gpio_free(g_music_drv.button_gpios[count]);
	}
}

static int music_request_buttons(void)
{
	int i;
	int ret;

	for (i = 0; i < BUTTON_COUNT; ++i)
	{
		g_music_drv.button_irqs[i] = -1;

		ret = gpio_request(g_music_drv.button_gpios[i], BUTTON_DEVICE_NAME);
		if (ret)
		{
			goto err;
		}

		ret = gpio_direction_input(g_music_drv.button_gpios[i]);
		if (ret)
		{
			gpio_free(g_music_drv.button_gpios[i]);
			goto err;
		}

		g_music_drv.button_irqs[i] = gpio_to_irq(g_music_drv.button_gpios[i]);
		if (g_music_drv.button_irqs[i] < 0)
		{
			ret = g_music_drv.button_irqs[i];
			gpio_free(g_music_drv.button_gpios[i]);
			goto err;
		}

		ret = request_irq(g_music_drv.button_irqs[i],
			music_button_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			BUTTON_DEVICE_NAME,
			&g_music_drv);
		if (ret)
		{
			gpio_free(g_music_drv.button_gpios[i]);
			g_music_drv.button_irqs[i] = -1;
			goto err;
		}
	}

	return 0;

err:
	music_release_buttons(i);
	return ret;
}

static int music_request_leds(void)
{
	int i;
	int ret;

	for (i = 0; i < LED_COUNT; ++i)
	{
		ret = gpio_request(g_music_drv.led_gpios[i], LED_DEVICE_NAME);
		if (ret)
		{
			goto err;
		}

		ret = gpio_direction_output(
			g_music_drv.led_gpios[i],
			g_music_drv.led_active_high ? 0 : 1);
		if (ret)
		{
			gpio_free(g_music_drv.led_gpios[i]);
			goto err;
		}
	}

	return 0;

err:
	music_release_leds(i);
	return ret;
}

int __init music_driver_init(void)
{
	int ret;
	int i;

	if (button_gpio_count != BUTTON_COUNT || led_gpio_count != LED_COUNT)
	{
		pr_err("music_gpio: expect %d button gpios and %d led gpios\n", BUTTON_COUNT, LED_COUNT);
		return -EINVAL;
	}

	memset(&g_music_drv, 0, sizeof(g_music_drv));
	init_waitqueue_head(&g_music_drv.button_waitq);
	spin_lock_init(&g_music_drv.button_lock);
	mutex_init(&g_music_drv.led_lock);
	timer_setup(&g_music_drv.debounce_timer, music_button_debounce_timer, 0);

	for (i = 0; i < BUTTON_COUNT; ++i)
	{
		g_music_drv.button_gpios[i] = button_gpios[i];
		g_music_drv.button_irqs[i] = -1;
		g_music_drv.button_state[i] = BUTTON_RELEASED_CHAR;
	}
	for (i = 0; i < LED_COUNT; ++i)
	{
		g_music_drv.led_gpios[i] = led_gpios[i];
	}
	g_music_drv.button_active_low = button_active_low;
	g_music_drv.led_active_high = led_active_high;
	g_music_drv.buttons_changed = false;

	ret = music_request_buttons();
	if (ret)
	{
		del_timer_sync(&g_music_drv.debounce_timer);
		return ret;
	}

	ret = music_request_leds();
	if (ret)
	{
		music_release_buttons(BUTTON_COUNT);
		del_timer_sync(&g_music_drv.debounce_timer);
		return ret;
	}

	g_music_drv.buttons_miscdev.minor = MISC_DYNAMIC_MINOR;
	g_music_drv.buttons_miscdev.name = BUTTON_DEVICE_NAME;
	g_music_drv.buttons_miscdev.fops = &buttons_fops;

	g_music_drv.leds_miscdev.minor = MISC_DYNAMIC_MINOR;
	g_music_drv.leds_miscdev.name = LED_DEVICE_NAME;
	g_music_drv.leds_miscdev.fops = &leds_fops;

	ret = misc_register(&g_music_drv.buttons_miscdev);
	if (ret)
	{
		music_release_leds(LED_COUNT);
		music_release_buttons(BUTTON_COUNT);
		del_timer_sync(&g_music_drv.debounce_timer);
		return ret;
	}

	ret = misc_register(&g_music_drv.leds_miscdev);
	if (ret)
	{
		misc_deregister(&g_music_drv.buttons_miscdev);
		music_release_leds(LED_COUNT);
		music_release_buttons(BUTTON_COUNT);
		del_timer_sync(&g_music_drv.debounce_timer);
		return ret;
	}

	music_update_button_state(false);

	pr_info("music_gpio: driver loaded, /dev/%s and /dev/%s ready\n",
		BUTTON_DEVICE_NAME, LED_DEVICE_NAME);
	return 0;
}

void __exit music_driver_exit(void)
{
	misc_deregister(&g_music_drv.leds_miscdev);
	misc_deregister(&g_music_drv.buttons_miscdev);
	del_timer_sync(&g_music_drv.debounce_timer);
	music_release_leds(LED_COUNT);
	music_release_buttons(BUTTON_COUNT);
	pr_info("music_gpio: driver unloaded\n");
}

module_init(music_driver_init);
module_exit(music_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YU");
MODULE_DESCRIPTION("GPIO buttons and leds driver for music player");
