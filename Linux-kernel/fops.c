#include "device.h"

static int buttons_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	return 0;
}

static int buttons_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	return 0;
}

static ssize_t buttons_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	unsigned long flags;
	unsigned char snapshot[BUTTON_COUNT];
	bool ready;
	int ret;

	(void)f_pos;

	if (count < BUTTON_COUNT)
	{
		return -EINVAL;
	}

	if (filp->f_flags & O_NONBLOCK)
	{
		spin_lock_irqsave(&g_music_drv.button_lock, flags);
		ready = g_music_drv.buttons_changed;
		spin_unlock_irqrestore(&g_music_drv.button_lock, flags);
		if (!ready)
		{
			return -EAGAIN;
		}
	}
	else
	{
		ret = wait_event_interruptible(g_music_drv.button_waitq, g_music_drv.buttons_changed);
		if (ret)
		{
			return ret;
		}
	}

	spin_lock_irqsave(&g_music_drv.button_lock, flags);
	memcpy(snapshot, g_music_drv.button_state, sizeof(snapshot));
	g_music_drv.buttons_changed = false;
	spin_unlock_irqrestore(&g_music_drv.button_lock, flags);

	if (copy_to_user(buf, snapshot, sizeof(snapshot)))
	{
		return -EFAULT;
	}

	return sizeof(snapshot);
}

static __poll_t buttons_poll(struct file *filp, poll_table *wait)
{
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filp, &g_music_drv.button_waitq, wait);

	spin_lock_irqsave(&g_music_drv.button_lock, flags);
	if (g_music_drv.buttons_changed)
	{
		mask |= POLLIN | POLLRDNORM;
	}
	spin_unlock_irqrestore(&g_music_drv.button_lock, flags);

	return mask;
}

static int leds_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	return 0;
}

static int leds_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	return 0;
}

static long leds_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	(void)filp;

	if (cmd != LED_OFF_CMD && cmd != LED_ON_CMD)
	{
		return -ENOTTY;
	}

	return music_set_led((unsigned int)arg, cmd == LED_ON_CMD);
}

const struct file_operations buttons_fops = {
	.owner = THIS_MODULE,
	.open = buttons_open,
	.release = buttons_release,
	.read = buttons_read,
	.poll = buttons_poll,
	.llseek = no_llseek,
};

const struct file_operations leds_fops = {
	.owner = THIS_MODULE,
	.open = leds_open,
	.release = leds_release,
	.unlocked_ioctl = leds_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = leds_ioctl,
#endif
	.llseek = no_llseek,
};
