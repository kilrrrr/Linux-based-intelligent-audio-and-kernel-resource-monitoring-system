#ifndef _MUSIC_GPIO_DEVICE_H_
#define _MUSIC_GPIO_DEVICE_H_

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define BUTTON_DEVICE_NAME "buttons"
#define LED_DEVICE_NAME    "leds"

#define BUTTON_COUNT 6
#define LED_COUNT    4

#define DEBOUNCE_MS 20

#define BUTTON_PRESSED_CHAR  '1'
#define BUTTON_RELEASED_CHAR '0'

#define LED_OFF_CMD 0
#define LED_ON_CMD  1

struct music_gpio_drv
{
	wait_queue_head_t button_waitq;
	spinlock_t button_lock;
	struct mutex led_lock;
	struct timer_list debounce_timer;
	struct miscdevice buttons_miscdev;
	struct miscdevice leds_miscdev;
	unsigned char button_state[BUTTON_COUNT];
	bool buttons_changed;
	int button_irqs[BUTTON_COUNT];
	int button_gpios[BUTTON_COUNT];
	int led_gpios[LED_COUNT];
	bool button_active_low;
	bool led_active_high;
};

extern struct music_gpio_drv g_music_drv;

int music_driver_init(void);
void music_driver_exit(void);
irqreturn_t music_button_irq_handler(int irq, void *dev_id);
void music_button_debounce_timer(struct timer_list *t);
int music_set_led(unsigned int which, bool on);

extern const struct file_operations buttons_fops;
extern const struct file_operations leds_fops;

#endif
