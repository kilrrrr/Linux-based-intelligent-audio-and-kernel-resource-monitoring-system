#include "res_guard.h"
#include <linux/cdev.h>
#include <linux/class.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/sysinfo.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define RES_GUARD_DEVICE_NAME "res_guard"
#define RES_GUARD_CLASS_NAME  "res_guard"
#define RES_GUARD_PROC_DIR    "res_guard"
#define RES_GUARD_PROC_STATUS "status"

#define RES_GUARD_DEFAULT_SAMPLE_PERIOD_MS      1000
#define RES_GUARD_DEFAULT_CPU_HIGH_PERMILLE      850
#define RES_GUARD_DEFAULT_MEM_LOW_KB           16384
#define RES_GUARD_DEFAULT_UNDERRUN_THRESHOLD       1
#define RES_GUARD_DEFAULT_RECOVERY_FAIL_THRESHOLD  1
#define RES_GUARD_DEFAULT_UART_DROP_THRESHOLD      1
#define RES_GUARD_DEFAULT_WAIT_TIMEOUT_THRESHOLD   1

struct res_guard_dev {
	struct cdev cdev;
	dev_t devno;
	struct class *cls;
	struct device *dev;

	struct delayed_work sample_work;
	wait_queue_head_t event_wq;
	spinlock_t lock;

	unsigned int event_pending;
	unsigned int sample_period_ms;
	u64 prev_total_time;
	u64 prev_idle_time;
	bool initialized;

	struct res_guard_snapshot snapshot;
	struct res_guard_thresholds thresholds;
};

static struct res_guard_dev g_res_guard;
static unsigned int sample_period_ms = RES_GUARD_DEFAULT_SAMPLE_PERIOD_MS;
module_param(sample_period_ms, uint, 0644);
MODULE_PARM_DESC(sample_period_ms, "Sampling period in milliseconds");

static u64 res_guard_now_ms(void)
{
	return div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
}

static const char *res_guard_event_name(unsigned int type)
{
	switch (type)
	{
	case RES_GUARD_EVT_CPU_HIGH:
		return "CPU_HIGH";
	case RES_GUARD_EVT_MEM_LOW:
		return "MEM_LOW";
	case RES_GUARD_EVT_AUDIO_UNDERRUN:
		return "AUDIO_UNDERRUN";
	case RES_GUARD_EVT_AUDIO_RECOVERY_FAIL:
		return "AUDIO_RECOVERY_FAIL";
	case RES_GUARD_EVT_UART_BACKLOG:
		return "UART_BACKLOG";
	case RES_GUARD_EVT_IRQ_BURST:
		return "IRQ_BURST";
	case RES_GUARD_EVT_WAIT_TIMEOUT:
		return "WAIT_TIMEOUT";
	default:
		return "NONE";
	}
}

static unsigned int res_guard_pages_to_kb(unsigned long pages)
{
	return (unsigned int)(pages << (PAGE_SHIFT - 10));
}

static void res_guard_collect_cpu_times(u64 *total_time, u64 *idle_time)
{
	int cpu;

	*total_time = 0;
	*idle_time = 0;

	for_each_online_cpu(cpu)
	{
		struct kernel_cpustat *cpustat = &kcpustat_cpu(cpu);
		int stat;

		for (stat = 0; stat < NR_STATS; ++stat)
		{
			*total_time += cpustat->cpustat[stat];
		}

		*idle_time += cpustat->cpustat[CPUTIME_IDLE];
#ifdef CPUTIME_IOWAIT
		*idle_time += cpustat->cpustat[CPUTIME_IOWAIT];
#endif
	}
}

static void res_guard_set_defaults(struct res_guard_dev *guard)
{
	guard->sample_period_ms = sample_period_ms;
	guard->thresholds.cpu_high_permille = RES_GUARD_DEFAULT_CPU_HIGH_PERMILLE;
	guard->thresholds.mem_low_kb = RES_GUARD_DEFAULT_MEM_LOW_KB;
	guard->thresholds.underrun_threshold = RES_GUARD_DEFAULT_UNDERRUN_THRESHOLD;
	guard->thresholds.recovery_fail_threshold = RES_GUARD_DEFAULT_RECOVERY_FAIL_THRESHOLD;
	guard->thresholds.uart_drop_threshold = RES_GUARD_DEFAULT_UART_DROP_THRESHOLD;
	guard->thresholds.wait_timeout_threshold = RES_GUARD_DEFAULT_WAIT_TIMEOUT_THRESHOLD;
}

static void __res_guard_raise_event_locked(struct res_guard_dev *guard,
	unsigned int event_type,
	unsigned int alarm_level)
{
	guard->event_pending = 1;
	guard->snapshot.event_pending = 1;
	if (alarm_level > guard->snapshot.current_alarm_level)
	{
		guard->snapshot.current_alarm_level = alarm_level;
	}
	guard->snapshot.last_event_type = event_type;
	guard->snapshot.last_event_ts_ms = res_guard_now_ms();
}

static void res_guard_raise_event(struct res_guard_dev *guard,
	unsigned int event_type,
	unsigned int alarm_level)
{
	unsigned long flags;

	spin_lock_irqsave(&guard->lock, flags);
	__res_guard_raise_event_locked(guard, event_type, alarm_level);
	spin_unlock_irqrestore(&guard->lock, flags);

	wake_up_interruptible(&guard->event_wq);
}

static void res_guard_ack_event_locked(struct res_guard_dev *guard)
{
	guard->event_pending = 0;
	guard->snapshot.event_pending = 0;
	guard->snapshot.current_alarm_level = 0;
}

static void res_guard_copy_snapshot(struct res_guard_snapshot *snapshot)
{
	unsigned long flags;

	spin_lock_irqsave(&g_res_guard.lock, flags);
	*snapshot = g_res_guard.snapshot;
	spin_unlock_irqrestore(&g_res_guard.lock, flags);
}

static void res_guard_copy_thresholds(struct res_guard_thresholds *thresholds)
{
	unsigned long flags;

	spin_lock_irqsave(&g_res_guard.lock, flags);
	*thresholds = g_res_guard.thresholds;
	spin_unlock_irqrestore(&g_res_guard.lock, flags);
}

static void res_guard_clear_counters_locked(struct res_guard_dev *guard)
{
	memset(&guard->snapshot.counters, 0, sizeof(guard->snapshot.counters));
	guard->snapshot.last_event_type = RES_GUARD_EVT_NONE;
	guard->snapshot.last_event_ts_ms = 0;
	res_guard_ack_event_locked(guard);
}

static void res_guard_sample_workfn(struct work_struct *work)
{
	struct res_guard_dev *guard = container_of(to_delayed_work(work),
		struct res_guard_dev,
		sample_work);
	struct sysinfo info;
	unsigned long flags;
	u64 total_time;
	u64 idle_time;
	u64 delta_total;
	u64 delta_idle;
	unsigned int cpu_usage_permille = 0;
	unsigned int mem_free_kb;
	unsigned int mem_available_kb;
	unsigned int pending_event = RES_GUARD_EVT_NONE;

	res_guard_collect_cpu_times(&total_time, &idle_time);
	if (guard->prev_total_time != 0 && total_time > guard->prev_total_time)
	{
		delta_total = total_time - guard->prev_total_time;
		delta_idle = idle_time - guard->prev_idle_time;
		if (delta_total > 0 && delta_total > delta_idle)
		{
			cpu_usage_permille = (unsigned int)div_u64((delta_total - delta_idle) * 1000, delta_total);
		}
	}

	guard->prev_total_time = total_time;
	guard->prev_idle_time = idle_time;

	si_meminfo(&info);
	mem_free_kb = res_guard_pages_to_kb(info.freeram);
	mem_available_kb = res_guard_pages_to_kb(info.freeram + info.bufferram);

	spin_lock_irqsave(&guard->lock, flags);
	guard->snapshot.cpu_usage_permille = cpu_usage_permille;
	guard->snapshot.mem_free_kb = mem_free_kb;
	guard->snapshot.mem_available_kb = mem_available_kb;
	guard->snapshot.counters.sample_count++;

	if (guard->thresholds.cpu_high_permille > 0 &&
		cpu_usage_permille >= guard->thresholds.cpu_high_permille)
	{
		guard->snapshot.counters.cpu_high_count++;
		pending_event = RES_GUARD_EVT_CPU_HIGH;
	}

	if (guard->thresholds.mem_low_kb > 0 &&
		mem_available_kb <= guard->thresholds.mem_low_kb)
	{
		guard->snapshot.counters.mem_low_count++;
		pending_event = RES_GUARD_EVT_MEM_LOW;
	}
	spin_unlock_irqrestore(&guard->lock, flags);

	if (pending_event != RES_GUARD_EVT_NONE)
	{
		res_guard_raise_event(guard, pending_event, 1);
	}

	if (guard->initialized)
	{
		schedule_delayed_work(&guard->sample_work, msecs_to_jiffies(guard->sample_period_ms));
	}
}

void res_guard_report_irq(unsigned int type)
{
	unsigned long flags;

	(void)type;
	if (!g_res_guard.initialized)
	{
		return;
	}

	spin_lock_irqsave(&g_res_guard.lock, flags);
	g_res_guard.snapshot.counters.key_irq_count++;
	spin_unlock_irqrestore(&g_res_guard.lock, flags);
}
EXPORT_SYMBOL_GPL(res_guard_report_irq);

void res_guard_report_uart_drop(unsigned int bytes)
{
	unsigned long flags;
	bool raise = false;

	if (!g_res_guard.initialized)
	{
		return;
	}

	spin_lock_irqsave(&g_res_guard.lock, flags);
	g_res_guard.snapshot.counters.uart_drop_count += bytes ? bytes : 1;
	if (g_res_guard.thresholds.uart_drop_threshold > 0 &&
		g_res_guard.snapshot.counters.uart_drop_count >= g_res_guard.thresholds.uart_drop_threshold)
	{
		raise = true;
	}
	spin_unlock_irqrestore(&g_res_guard.lock, flags);

	if (raise)
	{
		res_guard_raise_event(&g_res_guard, RES_GUARD_EVT_UART_BACKLOG, 1);
	}
}
EXPORT_SYMBOL_GPL(res_guard_report_uart_drop);

void res_guard_report_wait_timeout(unsigned int type)
{
	unsigned long flags;
	bool raise = false;

	(void)type;
	if (!g_res_guard.initialized)
	{
		return;
	}

	spin_lock_irqsave(&g_res_guard.lock, flags);
	g_res_guard.snapshot.counters.wait_timeout_count++;
	if (g_res_guard.thresholds.wait_timeout_threshold > 0 &&
		g_res_guard.snapshot.counters.wait_timeout_count >= g_res_guard.thresholds.wait_timeout_threshold)
	{
		raise = true;
	}
	spin_unlock_irqrestore(&g_res_guard.lock, flags);

	if (raise)
	{
		res_guard_raise_event(&g_res_guard, RES_GUARD_EVT_WAIT_TIMEOUT, 1);
	}
}
EXPORT_SYMBOL_GPL(res_guard_report_wait_timeout);

void res_guard_report_audio_event(unsigned int type)
{
	unsigned long flags;
	unsigned int event_type = RES_GUARD_EVT_NONE;
	unsigned int alarm_level = 0;

	if (!g_res_guard.initialized)
	{
		return;
	}

	spin_lock_irqsave(&g_res_guard.lock, flags);
	switch (type)
	{
	case RES_GUARD_AUDIO_EVT_UNDERRUN:
		g_res_guard.snapshot.counters.audio_underrun_count++;
		if (g_res_guard.thresholds.underrun_threshold > 0 &&
			g_res_guard.snapshot.counters.audio_underrun_count >= g_res_guard.thresholds.underrun_threshold)
		{
			event_type = RES_GUARD_EVT_AUDIO_UNDERRUN;
			alarm_level = 1;
		}
		break;
	case RES_GUARD_AUDIO_EVT_RECOVERY_OK:
		g_res_guard.snapshot.counters.audio_recovery_ok_count++;
		break;
	case RES_GUARD_AUDIO_EVT_RECOVERY_FAIL:
		g_res_guard.snapshot.counters.audio_recovery_fail_count++;
		if (g_res_guard.thresholds.recovery_fail_threshold > 0 &&
			g_res_guard.snapshot.counters.audio_recovery_fail_count >= g_res_guard.thresholds.recovery_fail_threshold)
		{
			event_type = RES_GUARD_EVT_AUDIO_RECOVERY_FAIL;
			alarm_level = 2;
		}
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&g_res_guard.lock, flags);

	if (event_type != RES_GUARD_EVT_NONE)
	{
		res_guard_raise_event(&g_res_guard, event_type, alarm_level);
	}
}
EXPORT_SYMBOL_GPL(res_guard_report_audio_event);

static int res_guard_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	filp->private_data = &g_res_guard;
	return 0;
}

static int res_guard_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	return 0;
}

static long res_guard_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct res_guard_dev *guard = filp->private_data;
	unsigned long flags;

	if (_IOC_TYPE(cmd) != RES_GUARD_IOC_MAGIC)
	{
		return -ENOTTY;
	}

	switch (cmd)
	{
	case RES_GUARD_IOC_GET_SNAPSHOT:
	{
		struct res_guard_snapshot snapshot;

		res_guard_copy_snapshot(&snapshot);
		if (copy_to_user((void __user *)arg, &snapshot, sizeof(snapshot)))
		{
			return -EFAULT;
		}
		return 0;
	}
	case RES_GUARD_IOC_GET_THRESHOLDS:
	{
		struct res_guard_thresholds thresholds;

		res_guard_copy_thresholds(&thresholds);
		if (copy_to_user((void __user *)arg, &thresholds, sizeof(thresholds)))
		{
			return -EFAULT;
		}
		return 0;
	}
	case RES_GUARD_IOC_SET_THRESHOLDS:
	{
		struct res_guard_thresholds thresholds;

		if (copy_from_user(&thresholds, (void __user *)arg, sizeof(thresholds)))
		{
			return -EFAULT;
		}

		spin_lock_irqsave(&guard->lock, flags);
		guard->thresholds = thresholds;
		spin_unlock_irqrestore(&guard->lock, flags);
		return 0;
	}
	case RES_GUARD_IOC_CLEAR_COUNTERS:
		spin_lock_irqsave(&guard->lock, flags);
		res_guard_clear_counters_locked(guard);
		spin_unlock_irqrestore(&guard->lock, flags);
		return 0;
	case RES_GUARD_IOC_REPORT_UNDERRUN:
		res_guard_report_audio_event(RES_GUARD_AUDIO_EVT_UNDERRUN);
		return 0;
	case RES_GUARD_IOC_REPORT_RECOVERY_OK:
		res_guard_report_audio_event(RES_GUARD_AUDIO_EVT_RECOVERY_OK);
		return 0;
	case RES_GUARD_IOC_REPORT_RECOVERY_FAIL:
		res_guard_report_audio_event(RES_GUARD_AUDIO_EVT_RECOVERY_FAIL);
		return 0;
	case RES_GUARD_IOC_ACK_EVENT:
		spin_lock_irqsave(&guard->lock, flags);
		res_guard_ack_event_locked(guard);
		spin_unlock_irqrestore(&guard->lock, flags);
		return 0;
	default:
		return -ENOTTY;
	}
}

static __poll_t res_guard_poll(struct file *filp, poll_table *wait)
{
	struct res_guard_dev *guard = filp->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(filp, &guard->event_wq, wait);

	spin_lock_irqsave(&guard->lock, flags);
	if (guard->event_pending)
	{
		mask |= POLLIN | POLLRDNORM;
	}
	spin_unlock_irqrestore(&guard->lock, flags);

	return mask;
}

static const struct file_operations res_guard_fops = {
	.owner = THIS_MODULE,
	.open = res_guard_open,
	.release = res_guard_release,
	.unlocked_ioctl = res_guard_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = res_guard_ioctl,
#endif
	.poll = res_guard_poll,
	.llseek = no_llseek,
};

static int res_guard_proc_show(struct seq_file *m, void *v)
{
	struct res_guard_snapshot snapshot;

	(void)v;
	res_guard_copy_snapshot(&snapshot);

	seq_printf(m, "cpu_usage_permille: %u\n", snapshot.cpu_usage_permille);
	seq_printf(m, "mem_free_kb: %u\n", snapshot.mem_free_kb);
	seq_printf(m, "mem_available_kb: %u\n", snapshot.mem_available_kb);
	seq_printf(m, "event_pending: %u\n", snapshot.event_pending);
	seq_printf(m, "current_alarm_level: %u\n", snapshot.current_alarm_level);
	seq_printf(m, "last_event_type: %s\n", res_guard_event_name(snapshot.last_event_type));
	seq_printf(m, "last_event_ts_ms: %llu\n", snapshot.last_event_ts_ms);
	seq_printf(m, "sample_count: %llu\n", snapshot.counters.sample_count);
	seq_printf(m, "key_irq_count: %llu\n", snapshot.counters.key_irq_count);
	seq_printf(m, "uart_rx_count: %llu\n", snapshot.counters.uart_rx_count);
	seq_printf(m, "uart_drop_count: %llu\n", snapshot.counters.uart_drop_count);
	seq_printf(m, "wait_timeout_count: %llu\n", snapshot.counters.wait_timeout_count);
	seq_printf(m, "audio_underrun_count: %llu\n", snapshot.counters.audio_underrun_count);
	seq_printf(m, "audio_recovery_ok_count: %llu\n", snapshot.counters.audio_recovery_ok_count);
	seq_printf(m, "audio_recovery_fail_count: %llu\n", snapshot.counters.audio_recovery_fail_count);
	seq_printf(m, "cpu_high_count: %llu\n", snapshot.counters.cpu_high_count);
	seq_printf(m, "mem_low_count: %llu\n", snapshot.counters.mem_low_count);

	return 0;
}

static int res_guard_proc_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, res_guard_proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops res_guard_proc_ops = {
	.proc_open = res_guard_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations res_guard_proc_ops = {
	.owner = THIS_MODULE,
	.open = res_guard_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

#define RES_GUARD_CLASS_ATTR_RW(_name, _field) \
static ssize_t _name##_show(struct class *class, struct class_attribute *attr, char *buf) \
{ \
	struct res_guard_thresholds thresholds; \
	(void)class; \
	(void)attr; \
	res_guard_copy_thresholds(&thresholds); \
	return scnprintf(buf, PAGE_SIZE, "%u\n", thresholds._field); \
} \
static ssize_t _name##_store(struct class *class, struct class_attribute *attr, const char *buf, size_t count) \
{ \
	unsigned int value; \
	unsigned long flags; \
	(void)class; \
	(void)attr; \
	if (kstrtouint(buf, 0, &value)) \
	{ \
		return -EINVAL; \
	} \
	spin_lock_irqsave(&g_res_guard.lock, flags); \
	g_res_guard.thresholds._field = value; \
	spin_unlock_irqrestore(&g_res_guard.lock, flags); \
	return count; \
} \
static CLASS_ATTR_RW(_name)

RES_GUARD_CLASS_ATTR_RW(cpu_high_permille, cpu_high_permille);
RES_GUARD_CLASS_ATTR_RW(mem_low_kb, mem_low_kb);
RES_GUARD_CLASS_ATTR_RW(underrun_threshold, underrun_threshold);
RES_GUARD_CLASS_ATTR_RW(recovery_fail_threshold, recovery_fail_threshold);
RES_GUARD_CLASS_ATTR_RW(uart_drop_threshold, uart_drop_threshold);
RES_GUARD_CLASS_ATTR_RW(wait_timeout_threshold, wait_timeout_threshold);

static struct class_attribute *res_guard_class_attrs[] = {
	&class_attr_cpu_high_permille,
	&class_attr_mem_low_kb,
	&class_attr_underrun_threshold,
	&class_attr_recovery_fail_threshold,
	&class_attr_uart_drop_threshold,
	&class_attr_wait_timeout_threshold,
};

static int res_guard_create_sysfs(struct class *cls)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(res_guard_class_attrs); ++i)
	{
		ret = class_create_file(cls, res_guard_class_attrs[i]);
		if (ret)
		{
			while (i-- > 0)
			{
				class_remove_file(cls, res_guard_class_attrs[i]);
			}
			return ret;
		}
	}

	return 0;
}

static void res_guard_remove_sysfs(struct class *cls)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(res_guard_class_attrs); ++i)
	{
		class_remove_file(cls, res_guard_class_attrs[i]);
	}
}

static int __init res_guard_init(void)
{
	struct res_guard_dev *guard = &g_res_guard;
	struct proc_dir_entry *proc_dir;
	int ret;

	memset(guard, 0, sizeof(*guard));
	spin_lock_init(&guard->lock);
	init_waitqueue_head(&guard->event_wq);
	INIT_DELAYED_WORK(&guard->sample_work, res_guard_sample_workfn);
	res_guard_set_defaults(guard);

	ret = alloc_chrdev_region(&guard->devno, 0, 1, RES_GUARD_DEVICE_NAME);
	if (ret)
	{
		return ret;
	}

	cdev_init(&guard->cdev, &res_guard_fops);
	guard->cdev.owner = THIS_MODULE;
	ret = cdev_add(&guard->cdev, guard->devno, 1);
	if (ret)
	{
		unregister_chrdev_region(guard->devno, 1);
		return ret;
	}

	guard->cls = class_create(THIS_MODULE, RES_GUARD_CLASS_NAME);
	if (IS_ERR(guard->cls))
	{
		ret = PTR_ERR(guard->cls);
		cdev_del(&guard->cdev);
		unregister_chrdev_region(guard->devno, 1);
		return ret;
	}

	ret = res_guard_create_sysfs(guard->cls);
	if (ret)
	{
		class_destroy(guard->cls);
		cdev_del(&guard->cdev);
		unregister_chrdev_region(guard->devno, 1);
		return ret;
	}

	guard->dev = device_create(guard->cls, NULL, guard->devno, NULL, RES_GUARD_DEVICE_NAME);
	if (IS_ERR(guard->dev))
	{
		ret = PTR_ERR(guard->dev);
		res_guard_remove_sysfs(guard->cls);
		class_destroy(guard->cls);
		cdev_del(&guard->cdev);
		unregister_chrdev_region(guard->devno, 1);
		return ret;
	}

	proc_dir = proc_mkdir(RES_GUARD_PROC_DIR, NULL);
	if (proc_dir == NULL)
	{
		device_destroy(guard->cls, guard->devno);
		res_guard_remove_sysfs(guard->cls);
		class_destroy(guard->cls);
		cdev_del(&guard->cdev);
		unregister_chrdev_region(guard->devno, 1);
		return -ENOMEM;
	}

	if (proc_create(RES_GUARD_PROC_STATUS, 0444, proc_dir, &res_guard_proc_ops) == NULL)
	{
		remove_proc_subtree(RES_GUARD_PROC_DIR, NULL);
		device_destroy(guard->cls, guard->devno);
		res_guard_remove_sysfs(guard->cls);
		class_destroy(guard->cls);
		cdev_del(&guard->cdev);
		unregister_chrdev_region(guard->devno, 1);
		return -ENOMEM;
	}

	guard->initialized = true;
	schedule_delayed_work(&guard->sample_work, msecs_to_jiffies(guard->sample_period_ms));

	pr_info("res_guard: loaded, /dev/%s ready\n", RES_GUARD_DEVICE_NAME);
	return 0;
}

static void __exit res_guard_exit(void)
{
	struct res_guard_dev *guard = &g_res_guard;

	guard->initialized = false;
	cancel_delayed_work_sync(&guard->sample_work);
	remove_proc_subtree(RES_GUARD_PROC_DIR, NULL);
	if (guard->dev)
	{
		device_destroy(guard->cls, guard->devno);
	}
	if (guard->cls)
	{
		res_guard_remove_sysfs(guard->cls);
		class_destroy(guard->cls);
	}
	cdev_del(&guard->cdev);
	unregister_chrdev_region(guard->devno, 1);
	pr_info("res_guard: unloaded\n");
}

module_init(res_guard_init);
module_exit(res_guard_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YU");
MODULE_DESCRIPTION("i.MX6ULL resource guard kernel module");
