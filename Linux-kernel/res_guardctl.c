#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "res_guard.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define RES_GUARDCTL_DEFAULT_DEVICE "/dev/res_guard"
#define RES_GUARDCTL_DEFAULT_TIMEOUT_MS 5000

struct res_guardctl_config {
	const char *device_path;
	int timeout_ms;
	bool auto_ack;
};

struct res_guardctl_threshold_field {
	const char *name;
	size_t offset;
};

static const struct res_guardctl_threshold_field g_res_guardctl_threshold_fields[] = {
	{"cpu_high_permille", offsetof(struct res_guard_thresholds, cpu_high_permille)},
	{"mem_low_kb", offsetof(struct res_guard_thresholds, mem_low_kb)},
	{"underrun_threshold", offsetof(struct res_guard_thresholds, underrun_threshold)},
	{"recovery_fail_threshold", offsetof(struct res_guard_thresholds, recovery_fail_threshold)},
	{"uart_drop_threshold", offsetof(struct res_guard_thresholds, uart_drop_threshold)},
	{"wait_timeout_threshold", offsetof(struct res_guard_thresholds, wait_timeout_threshold)},
};

static const char *res_guardctl_event_name(unsigned int type)
{
	switch (type) {
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

static const char *res_guardctl_alarm_name(unsigned int level)
{
	switch (level) {
	case 0:
		return "NORMAL";
	case 1:
		return "WARNING";
	case 2:
		return "CRITICAL";
	default:
		return "UNKNOWN";
	}
}

static void res_guardctl_print_snapshot(const struct res_guard_snapshot *snapshot)
{
	printf("snapshot:\n");
	printf("  cpu_permille=%u\n", snapshot->cpu_usage_permille);
	printf("  mem_free_kb=%u\n", snapshot->mem_free_kb);
	printf("  mem_available_kb=%u\n", snapshot->mem_available_kb);
	printf("  event_pending=%u\n", snapshot->event_pending);
	printf("  alarm=%s(%u)\n",
		res_guardctl_alarm_name(snapshot->current_alarm_level),
		snapshot->current_alarm_level);
	printf("  last_event=%s\n", res_guardctl_event_name(snapshot->last_event_type));
	printf("  last_event_ts_ms=%" PRIu64 "\n", (uint64_t)snapshot->last_event_ts_ms);
	printf("  counters.sample=%" PRIu64 "\n", (uint64_t)snapshot->counters.sample_count);
	printf("  counters.key_irq=%" PRIu64 "\n", (uint64_t)snapshot->counters.key_irq_count);
	printf("  counters.uart_rx=%" PRIu64 "\n", (uint64_t)snapshot->counters.uart_rx_count);
	printf("  counters.uart_drop=%" PRIu64 "\n", (uint64_t)snapshot->counters.uart_drop_count);
	printf("  counters.wait_timeout=%" PRIu64 "\n", (uint64_t)snapshot->counters.wait_timeout_count);
	printf("  counters.audio_underrun=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_underrun_count);
	printf("  counters.audio_recovery_ok=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_recovery_ok_count);
	printf("  counters.audio_recovery_fail=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_recovery_fail_count);
	printf("  counters.cpu_high=%" PRIu64 "\n", (uint64_t)snapshot->counters.cpu_high_count);
	printf("  counters.mem_low=%" PRIu64 "\n", (uint64_t)snapshot->counters.mem_low_count);
}

static void res_guardctl_print_thresholds(const struct res_guard_thresholds *thresholds)
{
	printf("thresholds:\n");
	printf("  cpu_high_permille=%u\n", thresholds->cpu_high_permille);
	printf("  mem_low_kb=%u\n", thresholds->mem_low_kb);
	printf("  underrun_threshold=%u\n", thresholds->underrun_threshold);
	printf("  recovery_fail_threshold=%u\n", thresholds->recovery_fail_threshold);
	printf("  uart_drop_threshold=%u\n", thresholds->uart_drop_threshold);
	printf("  wait_timeout_threshold=%u\n", thresholds->wait_timeout_threshold);
}

static void res_guardctl_print_status(const struct res_guardctl_config *cfg,
	const struct res_guard_thresholds *thresholds,
	const struct res_guard_snapshot *snapshot)
{
	printf("status:\n");
	printf("  device=%s\n", cfg->device_path);
	printf("  event_pending=%u\n", snapshot->event_pending);
	printf("  alarm=%s(%u)\n",
		res_guardctl_alarm_name(snapshot->current_alarm_level),
		snapshot->current_alarm_level);
	printf("  last_event=%s\n", res_guardctl_event_name(snapshot->last_event_type));
	printf("  cpu_permille=%u\n", snapshot->cpu_usage_permille);
	printf("  mem_available_kb=%u\n", snapshot->mem_available_kb);
	printf("  audio_underrun=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_underrun_count);
	printf("  audio_recovery_ok=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_recovery_ok_count);
	printf("  audio_recovery_fail=%" PRIu64 "\n", (uint64_t)snapshot->counters.audio_recovery_fail_count);
	res_guardctl_print_thresholds(thresholds);
}

static void res_guardctl_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options] <command> [args]\n"
		"\n"
		"Options:\n"
		"  -d, --device <path>    device path, default: %s\n"
		"  -t, --timeout <ms>     poll timeout for wait, default: %d\n"
		"  -a, --ack              auto-ack after wait receives an event\n"
		"  -h, --help             show this help\n"
		"\n"
		"Commands:\n"
		"  status                 show compact status + thresholds\n"
		"  snapshot               show full snapshot\n"
		"  thresholds             show thresholds\n"
		"  set <field> <value>    update one threshold field\n"
		"  ack                    ack current pending event\n"
		"  clear                  clear counters and pending event\n"
		"  inject <event> [cnt]   inject underrun/recovery-ok/recovery-fail\n"
		"  wait                   poll for event and print snapshot\n"
		"\n"
		"Threshold fields:\n"
		"  cpu_high_permille\n"
		"  mem_low_kb\n"
		"  underrun_threshold\n"
		"  recovery_fail_threshold\n"
		"  uart_drop_threshold\n"
		"  wait_timeout_threshold\n",
		prog,
		RES_GUARDCTL_DEFAULT_DEVICE,
		RES_GUARDCTL_DEFAULT_TIMEOUT_MS);
}

static int res_guardctl_parse_non_negative(const char *value)
{
	char *endptr = NULL;
	long result;

	errno = 0;
	result = strtol(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0' || result < 0 || result > INT32_MAX)
		return -1;

	return (int)result;
}

static int res_guardctl_open(const char *device_path)
{
	int fd;

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fprintf(stderr, "failed to open %s: %s\n", device_path, strerror(errno));

	return fd;
}

static int res_guardctl_get_snapshot(int fd, struct res_guard_snapshot *snapshot)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_SNAPSHOT, snapshot) < 0) {
		fprintf(stderr, "GET_SNAPSHOT failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardctl_get_thresholds(int fd, struct res_guard_thresholds *thresholds)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_THRESHOLDS, thresholds) < 0) {
		fprintf(stderr, "GET_THRESHOLDS failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardctl_set_thresholds(int fd, const struct res_guard_thresholds *thresholds)
{
	if (ioctl(fd, RES_GUARD_IOC_SET_THRESHOLDS, thresholds) < 0) {
		fprintf(stderr, "SET_THRESHOLDS failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardctl_repeat_ioctl(int fd, unsigned long request, int count, const char *label)
{
	int i;

	for (i = 0; i < count; ++i) {
		if (ioctl(fd, request) < 0) {
			fprintf(stderr, "%s failed at round %d: %s\n", label, i + 1, strerror(errno));
			return -1;
		}
	}

	printf("%s ok, count=%d\n", label, count);
	return 0;
}

static const struct res_guardctl_threshold_field *res_guardctl_find_threshold_field(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(g_res_guardctl_threshold_fields); ++i) {
		if (strcmp(name, g_res_guardctl_threshold_fields[i].name) == 0)
			return &g_res_guardctl_threshold_fields[i];
	}

	return NULL;
}

static int res_guardctl_set_field(int fd, const char *field_name, const char *value_str)
{
	const struct res_guardctl_threshold_field *field;
	struct res_guard_thresholds thresholds;
	int value;
	unsigned int *target;

	field = res_guardctl_find_threshold_field(field_name);
	if (field == NULL) {
		fprintf(stderr, "unknown threshold field: %s\n", field_name);
		return -1;
	}

	value = res_guardctl_parse_non_negative(value_str);
	if (value < 0) {
		fprintf(stderr, "invalid threshold value: %s\n", value_str);
		return -1;
	}

	if (res_guardctl_get_thresholds(fd, &thresholds) < 0)
		return -1;

	target = (unsigned int *)((char *)&thresholds + field->offset);
	*target = (unsigned int)value;

	if (res_guardctl_set_thresholds(fd, &thresholds) < 0)
		return -1;

	printf("set ok: %s=%u\n", field_name, *target);
	return 0;
}

static int res_guardctl_wait_event(int fd, const struct res_guardctl_config *cfg)
{
	struct pollfd pfd;
	struct res_guard_snapshot snapshot;
	int ret;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = POLLIN | POLLPRI;

	printf("waiting for event, timeout_ms=%d auto_ack=%s\n",
		cfg->timeout_ms,
		cfg->auto_ack ? "true" : "false");

	ret = poll(&pfd, 1, cfg->timeout_ms);
	if (ret < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		return -1;
	}

	if (ret == 0) {
		printf("poll timeout\n");
		return 0;
	}

	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		fprintf(stderr, "poll revents=0x%x\n", pfd.revents);
		return -1;
	}

	if (!(pfd.revents & (POLLIN | POLLPRI))) {
		fprintf(stderr, "unexpected poll revents=0x%x\n", pfd.revents);
		return -1;
	}

	if (res_guardctl_get_snapshot(fd, &snapshot) < 0)
		return -1;

	printf("event received\n");
	res_guardctl_print_snapshot(&snapshot);

	if (cfg->auto_ack && snapshot.event_pending) {
		if (ioctl(fd, RES_GUARD_IOC_ACK_EVENT) < 0) {
			fprintf(stderr, "ACK_EVENT failed: %s\n", strerror(errno));
			return -1;
		}
		printf("ACK_EVENT ok\n");
	}

	return 0;
}

static int res_guardctl_inject_event(int fd, const char *event_name, int count)
{
	if (strcmp(event_name, "underrun") == 0)
		return res_guardctl_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_UNDERRUN, count, "REPORT_UNDERRUN");

	if (strcmp(event_name, "recovery-ok") == 0)
		return res_guardctl_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_RECOVERY_OK, count, "REPORT_RECOVERY_OK");

	if (strcmp(event_name, "recovery-fail") == 0)
		return res_guardctl_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_RECOVERY_FAIL, count, "REPORT_RECOVERY_FAIL");

	fprintf(stderr, "unknown event: %s\n", event_name);
	return -1;
}

int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"device", required_argument, NULL, 'd'},
		{"timeout", required_argument, NULL, 't'},
		{"ack", no_argument, NULL, 'a'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};
	struct res_guardctl_config cfg;
	const char *command;
	int fd;
	int opt;
	int ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device_path = RES_GUARDCTL_DEFAULT_DEVICE;
	cfg.timeout_ms = RES_GUARDCTL_DEFAULT_TIMEOUT_MS;

	while ((opt = getopt_long(argc, argv, "d:t:ah", long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg.device_path = optarg;
			break;
		case 't':
			cfg.timeout_ms = res_guardctl_parse_non_negative(optarg);
			if (cfg.timeout_ms < 0) {
				fprintf(stderr, "invalid timeout: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'a':
			cfg.auto_ack = true;
			break;
		case 'h':
			res_guardctl_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			res_guardctl_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		res_guardctl_usage(argv[0]);
		return EXIT_FAILURE;
	}

	command = argv[optind++];
	fd = res_guardctl_open(cfg.device_path);
	if (fd < 0)
		return EXIT_FAILURE;

	ret = EXIT_FAILURE;

	if (strcmp(command, "status") == 0) {
		struct res_guard_snapshot snapshot;
		struct res_guard_thresholds thresholds;

		if (res_guardctl_get_thresholds(fd, &thresholds) == 0 &&
			res_guardctl_get_snapshot(fd, &snapshot) == 0) {
			res_guardctl_print_status(&cfg, &thresholds, &snapshot);
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "snapshot") == 0) {
		struct res_guard_snapshot snapshot;

		if (res_guardctl_get_snapshot(fd, &snapshot) == 0) {
			res_guardctl_print_snapshot(&snapshot);
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "thresholds") == 0) {
		struct res_guard_thresholds thresholds;

		if (res_guardctl_get_thresholds(fd, &thresholds) == 0) {
			res_guardctl_print_thresholds(&thresholds);
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "set") == 0) {
		if (optind + 1 >= argc) {
			fprintf(stderr, "set requires <field> <value>\n");
		} else if (res_guardctl_set_field(fd, argv[optind], argv[optind + 1]) == 0) {
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "ack") == 0) {
		if (ioctl(fd, RES_GUARD_IOC_ACK_EVENT) == 0) {
			printf("ACK_EVENT ok\n");
			ret = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "ACK_EVENT failed: %s\n", strerror(errno));
		}
	} else if (strcmp(command, "clear") == 0) {
		if (ioctl(fd, RES_GUARD_IOC_CLEAR_COUNTERS) == 0) {
			printf("CLEAR_COUNTERS ok\n");
			ret = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "CLEAR_COUNTERS failed: %s\n", strerror(errno));
		}
	} else if (strcmp(command, "inject") == 0) {
		int count;

		if (optind >= argc) {
			fprintf(stderr, "inject requires <event> [count]\n");
		} else {
			count = 1;
			if (optind + 1 < argc) {
				count = res_guardctl_parse_non_negative(argv[optind + 1]);
				if (count <= 0) {
					fprintf(stderr, "invalid count: %s\n", argv[optind + 1]);
					close(fd);
					return EXIT_FAILURE;
				}
			}

			if (res_guardctl_inject_event(fd, argv[optind], count) == 0)
				ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "wait") == 0) {
		if (res_guardctl_wait_event(fd, &cfg) == 0)
			ret = EXIT_SUCCESS;
	} else {
		fprintf(stderr, "unknown command: %s\n", command);
		res_guardctl_usage(argv[0]);
	}

	close(fd);
	return ret;
}
