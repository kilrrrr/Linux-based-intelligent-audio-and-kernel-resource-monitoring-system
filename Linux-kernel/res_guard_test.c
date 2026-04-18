#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
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

#define RES_GUARD_TEST_DEFAULT_DEVICE "/dev/res_guard"
#define RES_GUARD_TEST_DEFAULT_TIMEOUT_MS 5000

struct res_guard_test_config {
	const char *device_path;
	int timeout_ms;
	bool auto_ack;
};

static const char *res_guard_test_event_name(unsigned int type)
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

static const char *res_guard_test_alarm_name(unsigned int level)
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

static void res_guard_test_print_snapshot(const struct res_guard_snapshot *snapshot)
{
	printf("snapshot:\n");
	printf("  cpu_permille=%u\n", snapshot->cpu_usage_permille);
	printf("  mem_free_kb=%u\n", snapshot->mem_free_kb);
	printf("  mem_available_kb=%u\n", snapshot->mem_available_kb);
	printf("  event_pending=%u\n", snapshot->event_pending);
	printf("  alarm=%s(%u)\n",
		res_guard_test_alarm_name(snapshot->current_alarm_level),
		snapshot->current_alarm_level);
	printf("  last_event=%s\n", res_guard_test_event_name(snapshot->last_event_type));
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

static void res_guard_test_print_thresholds(const struct res_guard_thresholds *thresholds)
{
	printf("thresholds:\n");
	printf("  cpu_high_permille=%u\n", thresholds->cpu_high_permille);
	printf("  mem_low_kb=%u\n", thresholds->mem_low_kb);
	printf("  underrun_threshold=%u\n", thresholds->underrun_threshold);
	printf("  recovery_fail_threshold=%u\n", thresholds->recovery_fail_threshold);
	printf("  uart_drop_threshold=%u\n", thresholds->uart_drop_threshold);
	printf("  wait_timeout_threshold=%u\n", thresholds->wait_timeout_threshold);
}

static void res_guard_test_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options] <command> [count]\n"
		"\n"
		"Options:\n"
		"  -d, --device <path>   device path, default: %s\n"
		"  -t, --timeout <ms>    poll timeout for wait command, default: %d\n"
		"  -a, --ack             auto-ack after wait receives an event\n"
		"  -h, --help            show this help\n"
		"\n"
		"Commands:\n"
		"  snapshot              read and print current snapshot\n"
		"  thresholds            read and print thresholds\n"
		"  clear                 clear counters and pending event\n"
		"  underrun [count]      report audio underrun count times\n"
		"  recovery-ok [count]   report audio recovery success count times\n"
		"  recovery-fail [count] report audio recovery failure count times\n"
		"  ack                   ack current pending event\n"
		"  wait                  poll for event, then print snapshot\n",
		prog,
		RES_GUARD_TEST_DEFAULT_DEVICE,
		RES_GUARD_TEST_DEFAULT_TIMEOUT_MS);
}

static int res_guard_test_parse_non_negative(const char *value)
{
	char *endptr = NULL;
	long result;

	errno = 0;
	result = strtol(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0' || result < 0 || result > INT32_MAX)
		return -1;

	return (int)result;
}

static int res_guard_test_open(const char *device_path)
{
	int fd;

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fprintf(stderr, "failed to open %s: %s\n", device_path, strerror(errno));

	return fd;
}

static int res_guard_test_get_snapshot(int fd, struct res_guard_snapshot *snapshot)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_SNAPSHOT, snapshot) < 0) {
		fprintf(stderr, "GET_SNAPSHOT failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guard_test_get_thresholds(int fd, struct res_guard_thresholds *thresholds)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_THRESHOLDS, thresholds) < 0) {
		fprintf(stderr, "GET_THRESHOLDS failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guard_test_repeat_ioctl(int fd, unsigned long request, int count, const char *label)
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

static int res_guard_test_wait_event(int fd, const struct res_guard_test_config *cfg)
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

	if (res_guard_test_get_snapshot(fd, &snapshot) < 0)
		return -1;

	printf("event received\n");
	res_guard_test_print_snapshot(&snapshot);

	if (cfg->auto_ack && snapshot.event_pending) {
		if (ioctl(fd, RES_GUARD_IOC_ACK_EVENT) < 0) {
			fprintf(stderr, "ACK_EVENT failed: %s\n", strerror(errno));
			return -1;
		}
		printf("ACK_EVENT ok\n");
	}

	return 0;
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
	struct res_guard_test_config cfg;
	const char *command;
	int fd;
	int opt;
	int count = 1;
	int ret = EXIT_FAILURE;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device_path = RES_GUARD_TEST_DEFAULT_DEVICE;
	cfg.timeout_ms = RES_GUARD_TEST_DEFAULT_TIMEOUT_MS;

	while ((opt = getopt_long(argc, argv, "d:t:ah", long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg.device_path = optarg;
			break;
		case 't':
			cfg.timeout_ms = res_guard_test_parse_non_negative(optarg);
			if (cfg.timeout_ms < 0) {
				fprintf(stderr, "invalid timeout: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'a':
			cfg.auto_ack = true;
			break;
		case 'h':
			res_guard_test_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			res_guard_test_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		res_guard_test_usage(argv[0]);
		return EXIT_FAILURE;
	}

	command = argv[optind++];
	if (optind < argc) {
		count = res_guard_test_parse_non_negative(argv[optind]);
		if (count <= 0) {
			fprintf(stderr, "invalid count: %s\n", argv[optind]);
			return EXIT_FAILURE;
		}
	}

	fd = res_guard_test_open(cfg.device_path);
	if (fd < 0)
		return EXIT_FAILURE;

	if (strcmp(command, "snapshot") == 0) {
		struct res_guard_snapshot snapshot;

		if (res_guard_test_get_snapshot(fd, &snapshot) == 0) {
			res_guard_test_print_snapshot(&snapshot);
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "thresholds") == 0) {
		struct res_guard_thresholds thresholds;

		if (res_guard_test_get_thresholds(fd, &thresholds) == 0) {
			res_guard_test_print_thresholds(&thresholds);
			ret = EXIT_SUCCESS;
		}
	} else if (strcmp(command, "clear") == 0) {
		if (ioctl(fd, RES_GUARD_IOC_CLEAR_COUNTERS) == 0) {
			printf("CLEAR_COUNTERS ok\n");
			ret = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "CLEAR_COUNTERS failed: %s\n", strerror(errno));
		}
	} else if (strcmp(command, "underrun") == 0) {
		if (res_guard_test_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_UNDERRUN, count, "REPORT_UNDERRUN") == 0)
			ret = EXIT_SUCCESS;
	} else if (strcmp(command, "recovery-ok") == 0) {
		if (res_guard_test_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_RECOVERY_OK, count, "REPORT_RECOVERY_OK") == 0)
			ret = EXIT_SUCCESS;
	} else if (strcmp(command, "recovery-fail") == 0) {
		if (res_guard_test_repeat_ioctl(fd, RES_GUARD_IOC_REPORT_RECOVERY_FAIL, count, "REPORT_RECOVERY_FAIL") == 0)
			ret = EXIT_SUCCESS;
	} else if (strcmp(command, "ack") == 0) {
		if (ioctl(fd, RES_GUARD_IOC_ACK_EVENT) == 0) {
			printf("ACK_EVENT ok\n");
			ret = EXIT_SUCCESS;
		} else {
			fprintf(stderr, "ACK_EVENT failed: %s\n", strerror(errno));
		}
	} else if (strcmp(command, "wait") == 0) {
		if (res_guard_test_wait_event(fd, &cfg) == 0)
			ret = EXIT_SUCCESS;
	} else {
		fprintf(stderr, "unknown command: %s\n", command);
		res_guard_test_usage(argv[0]);
	}

	close(fd);
	return ret;
}
