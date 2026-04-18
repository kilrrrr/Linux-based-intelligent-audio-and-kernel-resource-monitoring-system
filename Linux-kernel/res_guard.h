#ifndef _RES_GUARD_H_
#define _RES_GUARD_H_

#include <linux/ioctl.h>
#include <linux/types.h>

enum res_guard_event_type {
	RES_GUARD_EVT_NONE = 0,
	RES_GUARD_EVT_CPU_HIGH,
	RES_GUARD_EVT_MEM_LOW,
	RES_GUARD_EVT_AUDIO_UNDERRUN,
	RES_GUARD_EVT_AUDIO_RECOVERY_FAIL,
	RES_GUARD_EVT_UART_BACKLOG,
	RES_GUARD_EVT_IRQ_BURST,
	RES_GUARD_EVT_WAIT_TIMEOUT,
};

enum res_guard_audio_event_type {
	RES_GUARD_AUDIO_EVT_UNDERRUN = 1,
	RES_GUARD_AUDIO_EVT_RECOVERY_OK,
	RES_GUARD_AUDIO_EVT_RECOVERY_FAIL,
};

struct res_guard_counters {
	__u64 sample_count;

	__u64 key_irq_count;
	__u64 uart_rx_count;
	__u64 uart_drop_count;
	__u64 wait_timeout_count;

	__u64 audio_underrun_count;
	__u64 audio_recovery_ok_count;
	__u64 audio_recovery_fail_count;

	__u64 cpu_high_count;
	__u64 mem_low_count;
};

struct res_guard_snapshot {
	__u32 cpu_usage_permille;
	__u32 mem_free_kb;
	__u32 mem_available_kb;

	__u32 event_pending;
	__u32 current_alarm_level;
	__u32 last_event_type;

	__u64 last_event_ts_ms;
	struct res_guard_counters counters;
};

struct res_guard_thresholds {
	__u32 cpu_high_permille;
	__u32 mem_low_kb;
	__u32 underrun_threshold;
	__u32 recovery_fail_threshold;
	__u32 uart_drop_threshold;
	__u32 wait_timeout_threshold;
};

#define RES_GUARD_IOC_MAGIC               'R'
#define RES_GUARD_IOC_GET_SNAPSHOT        _IOR(RES_GUARD_IOC_MAGIC, 0x01, struct res_guard_snapshot)
#define RES_GUARD_IOC_GET_THRESHOLDS      _IOR(RES_GUARD_IOC_MAGIC, 0x02, struct res_guard_thresholds)
#define RES_GUARD_IOC_SET_THRESHOLDS      _IOW(RES_GUARD_IOC_MAGIC, 0x03, struct res_guard_thresholds)
#define RES_GUARD_IOC_CLEAR_COUNTERS      _IO(RES_GUARD_IOC_MAGIC,  0x04)
#define RES_GUARD_IOC_REPORT_UNDERRUN     _IO(RES_GUARD_IOC_MAGIC,  0x05)
#define RES_GUARD_IOC_REPORT_RECOVERY_OK  _IO(RES_GUARD_IOC_MAGIC,  0x06)
#define RES_GUARD_IOC_REPORT_RECOVERY_FAIL _IO(RES_GUARD_IOC_MAGIC, 0x07)
#define RES_GUARD_IOC_ACK_EVENT           _IO(RES_GUARD_IOC_MAGIC,  0x08)

#ifdef __KERNEL__
void res_guard_report_irq(unsigned int type);
void res_guard_report_uart_drop(unsigned int bytes);
void res_guard_report_wait_timeout(unsigned int type);
void res_guard_report_audio_event(unsigned int type);
#endif

#endif
