#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "res_guard.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define RES_GUARDD_DEFAULT_DEVICE "/dev/res_guard"
#define RES_GUARDD_DEFAULT_TIMEOUT_MS 5000

struct res_guardd_config {
	const char *device_path;
	int poll_timeout_ms;
	bool auto_ack;
	bool dump_once;
	bool quiet;
};

static volatile sig_atomic_t g_stop = 0;

static void res_guardd_signal_handler(int signo)
{
	(void)signo;
	g_stop = 1;
}

static const char *res_guardd_event_name(unsigned int type)
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

static const char *res_guardd_alarm_name(unsigned int level)
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

static void res_guardd_format_now(char *buf, size_t buf_size)
{
	time_t now;
	struct tm tm_now;

	now = time(NULL);
	localtime_r(&now, &tm_now);
	strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void res_guardd_log_prefix(FILE *stream, const char *level)
{
	char now_buf[32];

	res_guardd_format_now(now_buf, sizeof(now_buf));
	fprintf(stream, "[%s] [%s] ", now_buf, level);
}

static void res_guardd_log_snapshot(const char *reason,
	const struct res_guard_snapshot *snapshot)
{
	res_guardd_log_prefix(stdout, "INFO");
	fprintf(stdout,
		"%s cpu_permille=%u mem_free=%uKB mem_avail=%uKB event_pending=%u alarm=%s(%u) last_event=%s last_event_ts_ms=%" PRIu64 "\n",
		reason,
		snapshot->cpu_usage_permille,
		snapshot->mem_free_kb,
		snapshot->mem_available_kb,
		snapshot->event_pending,
		res_guardd_alarm_name(snapshot->current_alarm_level),
		snapshot->current_alarm_level,
		res_guardd_event_name(snapshot->last_event_type),
		(uint64_t)snapshot->last_event_ts_ms);

	res_guardd_log_prefix(stdout, "INFO");
	fprintf(stdout,
		"counters sample=%" PRIu64 " irq=%" PRIu64 " uart_rx=%" PRIu64 " uart_drop=%" PRIu64 " wait_timeout=%" PRIu64 " underrun=%" PRIu64 " recovery_ok=%" PRIu64 " recovery_fail=%" PRIu64 " cpu_high=%" PRIu64 " mem_low=%" PRIu64 "\n",
		(uint64_t)snapshot->counters.sample_count,
		(uint64_t)snapshot->counters.key_irq_count,
		(uint64_t)snapshot->counters.uart_rx_count,
		(uint64_t)snapshot->counters.uart_drop_count,
		(uint64_t)snapshot->counters.wait_timeout_count,
		(uint64_t)snapshot->counters.audio_underrun_count,
		(uint64_t)snapshot->counters.audio_recovery_ok_count,
		(uint64_t)snapshot->counters.audio_recovery_fail_count,
		(uint64_t)snapshot->counters.cpu_high_count,
		(uint64_t)snapshot->counters.mem_low_count);
}

static void res_guardd_log_thresholds(const struct res_guard_thresholds *thresholds)
{
	res_guardd_log_prefix(stdout, "INFO");
	fprintf(stdout,
		"thresholds cpu_high_permille=%u mem_low_kb=%u underrun_threshold=%u recovery_fail_threshold=%u uart_drop_threshold=%u wait_timeout_threshold=%u\n",
		thresholds->cpu_high_permille,
		thresholds->mem_low_kb,
		thresholds->underrun_threshold,
		thresholds->recovery_fail_threshold,
		thresholds->uart_drop_threshold,
		thresholds->wait_timeout_threshold);
}

static void res_guardd_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options]\n"
		"  -d, --device <path>       device path, default: %s\n"
		"  -t, --timeout <ms>        poll timeout in ms, default: %d\n"
		"  -1, --dump-once           dump one snapshot and exit\n"
		"      --no-ack              do not ACK event automatically\n"
		"  -q, --quiet               disable periodic timeout snapshot logs\n"
		"  -h, --help                show this help\n",
		prog,
		RES_GUARDD_DEFAULT_DEVICE,
		RES_GUARDD_DEFAULT_TIMEOUT_MS);
}

static int res_guardd_parse_timeout(const char *value)
{
	char *endptr = NULL;
	long result;

	errno = 0;
	result = strtol(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0' || result < 0 || result > INT32_MAX)
		return -1;

	return (int)result;
}

static int res_guardd_open_device(const char *device_path)
{
	int fd;

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		res_guardd_log_prefix(stderr, "ERROR");
		fprintf(stderr, "failed to open %s: %s\n", device_path, strerror(errno));
		return -1;
	}

	return fd;
}

static int res_guardd_get_snapshot(int fd, struct res_guard_snapshot *snapshot)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_SNAPSHOT, snapshot) < 0) {
		res_guardd_log_prefix(stderr, "ERROR");
		fprintf(stderr, "GET_SNAPSHOT failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardd_get_thresholds(int fd, struct res_guard_thresholds *thresholds)
{
	if (ioctl(fd, RES_GUARD_IOC_GET_THRESHOLDS, thresholds) < 0) {
		res_guardd_log_prefix(stderr, "ERROR");
		fprintf(stderr, "GET_THRESHOLDS failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardd_ack_event(int fd)
{
	if (ioctl(fd, RES_GUARD_IOC_ACK_EVENT) < 0) {
		res_guardd_log_prefix(stderr, "ERROR");
		fprintf(stderr, "ACK_EVENT failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int res_guardd_wait_loop(int fd, const struct res_guardd_config *cfg)
{
	struct pollfd pfd;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = POLLIN | POLLPRI;

	while (!g_stop) {
		int ret;

		ret = poll(&pfd, 1, cfg->poll_timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			res_guardd_log_prefix(stderr, "ERROR");
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return -1;
		}

		if (ret == 0) {
			if (!cfg->quiet) {
				struct res_guard_snapshot snapshot;

				if (res_guardd_get_snapshot(fd, &snapshot) < 0)
					return -1;

				res_guardd_log_snapshot("periodic snapshot", &snapshot);
			}
			continue;
		}

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			res_guardd_log_prefix(stderr, "ERROR");
			fprintf(stderr, "poll revents=0x%x, device became unavailable\n", pfd.revents);
			return -1;
		}

		if (pfd.revents & (POLLIN | POLLPRI)) {
			struct res_guard_snapshot snapshot;

			if (res_guardd_get_snapshot(fd, &snapshot) < 0)
				return -1;

			res_guardd_log_snapshot("event received", &snapshot);

			if (cfg->auto_ack && snapshot.event_pending) {
				if (res_guardd_ack_event(fd) < 0)
					return -1;

				res_guardd_log_prefix(stdout, "INFO");
				fprintf(stdout, "event ACKed\n");
			}
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"device", required_argument, NULL, 'd'},
		{"timeout", required_argument, NULL, 't'},
		{"dump-once", no_argument, NULL, '1'},
		{"no-ack", no_argument, NULL, 1000},
		{"quiet", no_argument, NULL, 'q'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};
	struct sigaction sa;
	struct res_guardd_config cfg;
	struct res_guard_snapshot snapshot;
	struct res_guard_thresholds thresholds;
	int fd;
	int opt;
	int ret = EXIT_FAILURE;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device_path = RES_GUARDD_DEFAULT_DEVICE;
	cfg.poll_timeout_ms = RES_GUARDD_DEFAULT_TIMEOUT_MS;
	cfg.auto_ack = true;

	while ((opt = getopt_long(argc, argv, "d:t:1qh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg.device_path = optarg;
			break;
		case 't':
			cfg.poll_timeout_ms = res_guardd_parse_timeout(optarg);
			if (cfg.poll_timeout_ms < 0) {
				res_guardd_log_prefix(stderr, "ERROR");
				fprintf(stderr, "invalid timeout: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case '1':
			cfg.dump_once = true;
			break;
		case 'q':
			cfg.quiet = true;
			break;
		case 'h':
			res_guardd_usage(argv[0]);
			return EXIT_SUCCESS;
		case 1000:
			cfg.auto_ack = false;
			break;
		default:
			res_guardd_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = res_guardd_signal_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fd = res_guardd_open_device(cfg.device_path);
	if (fd < 0)
		return EXIT_FAILURE;

	if (res_guardd_get_thresholds(fd, &thresholds) < 0)
		goto out;
	res_guardd_log_thresholds(&thresholds);

	if (res_guardd_get_snapshot(fd, &snapshot) < 0)
		goto out;
	res_guardd_log_snapshot("startup snapshot", &snapshot);

	if (cfg.dump_once) {
		ret = EXIT_SUCCESS;
		goto out;
	}

	res_guardd_log_prefix(stdout, "INFO");
	fprintf(stdout,
		"res_guardd started, device=%s timeout_ms=%d auto_ack=%s quiet=%s\n",
		cfg.device_path,
		cfg.poll_timeout_ms,
		cfg.auto_ack ? "true" : "false",
		cfg.quiet ? "true" : "false");

	if (res_guardd_wait_loop(fd, &cfg) == 0)
		ret = EXIT_SUCCESS;

out:
	close(fd);

	res_guardd_log_prefix(stdout, "INFO");
	fprintf(stdout, "res_guardd exit\n");
	return ret;
}
