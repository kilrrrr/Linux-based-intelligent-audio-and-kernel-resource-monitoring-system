#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RES_GUARD_ALSA_DEFAULT_PCM_DEVICE "default"
#define RES_GUARD_ALSA_DEFAULT_GUARD_DEVICE "/dev/res_guard"
#define RES_GUARD_ALSA_DEFAULT_RATE 48000U
#define RES_GUARD_ALSA_DEFAULT_CHANNELS 2U
#define RES_GUARD_ALSA_DEFAULT_PERIOD_FRAMES 1024U
#define RES_GUARD_ALSA_DEFAULT_BUFFER_FRAMES 4096U
#define RES_GUARD_ALSA_DEFAULT_SECONDS 10U
#define RES_GUARD_ALSA_DEFAULT_TONE_HZ 440.0
#define RES_GUARD_ALSA_DEFAULT_VOLUME 0.20

struct res_guard_alsa_demo_config {
	const char *pcm_device;
	const char *guard_device;
	unsigned int rate;
	unsigned int channels;
	snd_pcm_uframes_t period_frames;
	snd_pcm_uframes_t buffer_frames;
	unsigned int seconds;
	double tone_hz;
	double volume;
	unsigned int stall_every;
	unsigned int stall_ms;
	bool use_guard;
	bool enable_recover;
	bool verbose;
};

struct res_guard_alsa_demo_stats {
	unsigned int write_calls;
	unsigned int underrun_reports;
	unsigned int recovery_ok_reports;
	unsigned int recovery_fail_reports;
	uint64_t frames_written;
};

static void res_guard_alsa_demo_usage(const char *prog)
{
	fprintf(stdout,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -D, --pcm-device <name>     ALSA playback device, default: %s\n"
		"  -g, --guard-device <path>   res_guard device, default: %s\n"
		"  -r, --rate <hz>             sample rate, default: %u\n"
		"  -c, --channels <n>          channel count, default: %u\n"
		"  -p, --period <frames>       period frames, default: %u\n"
		"  -b, --buffer <frames>       buffer frames, default: %u\n"
		"  -s, --seconds <n>           playback duration, default: %u\n"
		"  -f, --tone-hz <hz>          sine tone frequency, default: %.1f\n"
		"  -v, --volume <0..1>         sine tone volume, default: %.2f\n"
		"      --stall-every <n>       sleep every N writes to trigger underrun\n"
		"      --stall-ms <ms>         sleep duration when stall is enabled\n"
		"      --no-guard             do not report events to /dev/res_guard\n"
		"      --no-recover           do not call snd_pcm_recover/prepare\n"
		"  -q, --quiet                reduce normal logs\n"
		"  -h, --help                 show this help\n",
		prog,
		RES_GUARD_ALSA_DEFAULT_PCM_DEVICE,
		RES_GUARD_ALSA_DEFAULT_GUARD_DEVICE,
		RES_GUARD_ALSA_DEFAULT_RATE,
		RES_GUARD_ALSA_DEFAULT_CHANNELS,
		(unsigned int)RES_GUARD_ALSA_DEFAULT_PERIOD_FRAMES,
		(unsigned int)RES_GUARD_ALSA_DEFAULT_BUFFER_FRAMES,
		RES_GUARD_ALSA_DEFAULT_SECONDS,
		RES_GUARD_ALSA_DEFAULT_TONE_HZ,
		RES_GUARD_ALSA_DEFAULT_VOLUME);
}

static int res_guard_alsa_demo_parse_uint(const char *value, unsigned int *out)
{
	char *endptr = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0' || parsed > UINT32_MAX)
		return -1;

	*out = (unsigned int)parsed;
	return 0;
}

static int res_guard_alsa_demo_parse_double(const char *value, double *out)
{
	char *endptr = NULL;
	double parsed;

	errno = 0;
	parsed = strtod(value, &endptr);
	if (errno != 0 || endptr == value || *endptr != '\0')
		return -1;

	*out = parsed;
	return 0;
}

static void res_guard_alsa_demo_log(const struct res_guard_alsa_demo_config *cfg,
	const char *level,
	const char *fmt,
	...)
{
	va_list ap;

	if (cfg->verbose == false && strcmp(level, "INFO") == 0)
		return;

	fprintf(stdout, "[%s] ", level);

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	fputc('\n', stdout);
}

static int res_guard_alsa_demo_open_guard(const struct res_guard_alsa_demo_config *cfg)
{
	int fd;

	if (!cfg->use_guard)
		return -1;

	fd = open(cfg->guard_device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s: %s\n",
			cfg->guard_device,
			strerror(errno));
		return -1;
	}

	return fd;
}

static int res_guard_alsa_demo_report_event(int guard_fd,
	unsigned long request,
	const char *label,
	unsigned int *counter)
{
	if (guard_fd < 0)
		return 0;

	if (ioctl(guard_fd, request) < 0) {
		fprintf(stderr, "%s failed: %s\n", label, strerror(errno));
		return -1;
	}

	if (counter != NULL)
		(*counter)++;

	return 0;
}

static int res_guard_alsa_demo_configure_pcm(snd_pcm_t *pcm,
	struct res_guard_alsa_demo_config *cfg)
{
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_sw_params_t *sw_params;
	unsigned int rate;
	snd_pcm_uframes_t period_frames;
	snd_pcm_uframes_t buffer_frames;
	int err;
	int dir;

	rate = cfg->rate;
	period_frames = cfg->period_frames;
	buffer_frames = cfg->buffer_frames;

	if (buffer_frames < period_frames * 2)
		buffer_frames = period_frames * 4;

	snd_pcm_hw_params_alloca(&hw_params);
	snd_pcm_sw_params_alloca(&sw_params);

	if ((err = snd_pcm_hw_params_any(pcm, hw_params)) < 0) {
		fprintf(stderr, "snd_pcm_hw_params_any failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_access(pcm, hw_params,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "set_access failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_format(pcm, hw_params,
			SND_PCM_FORMAT_S16_LE)) < 0) {
		fprintf(stderr, "set_format failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_channels(pcm, hw_params,
			cfg->channels)) < 0) {
		fprintf(stderr, "set_channels failed: %s\n", snd_strerror(err));
		return -1;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, &dir)) < 0) {
		fprintf(stderr, "set_rate_near failed: %s\n", snd_strerror(err));
		return -1;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw_params,
			&period_frames, &dir)) < 0) {
		fprintf(stderr, "set_period_size_near failed: %s\n", snd_strerror(err));
		return -1;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params,
			&buffer_frames)) < 0) {
		fprintf(stderr, "set_buffer_size_near failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_hw_params(pcm, hw_params)) < 0) {
		fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_sw_params_current(pcm, sw_params)) < 0) {
		fprintf(stderr, "snd_pcm_sw_params_current failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw_params,
			period_frames)) < 0) {
		fprintf(stderr, "set_start_threshold failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw_params,
			period_frames)) < 0) {
		fprintf(stderr, "set_avail_min failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_sw_params(pcm, sw_params)) < 0) {
		fprintf(stderr, "snd_pcm_sw_params failed: %s\n", snd_strerror(err));
		return -1;
	}

	if ((err = snd_pcm_prepare(pcm)) < 0) {
		fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
		return -1;
	}

	cfg->rate = rate;
	cfg->period_frames = period_frames;
	cfg->buffer_frames = buffer_frames;
	return 0;
}

static void res_guard_alsa_demo_fill_tone(int16_t *buffer,
	snd_pcm_uframes_t frames,
	unsigned int channels,
	unsigned int rate,
	double tone_hz,
	double volume,
	double *phase)
{
	snd_pcm_uframes_t i;
	double step;

	step = (2.0 * M_PI * tone_hz) / (double)rate;
	if (volume < 0.0)
		volume = 0.0;
	if (volume > 1.0)
		volume = 1.0;

	for (i = 0; i < frames; ++i) {
		unsigned int ch;
		int16_t sample;
		double current_phase;

		current_phase = *phase;
		sample = (int16_t)(sin(current_phase) * (32767.0 * volume));

		for (ch = 0; ch < channels; ++ch)
			buffer[i * channels + ch] = sample;

		current_phase += step;
		if (current_phase >= 2.0 * M_PI)
			current_phase -= 2.0 * M_PI;
		*phase = current_phase;
	}
}

static int res_guard_alsa_demo_recover(snd_pcm_t *pcm,
	snd_pcm_sframes_t write_ret,
	const struct res_guard_alsa_demo_config *cfg,
	int guard_fd,
	struct res_guard_alsa_demo_stats *stats)
{
	int err;

	if (write_ret == -EPIPE || write_ret == -ESTRPIPE) {
		if (res_guard_alsa_demo_report_event(guard_fd,
				RES_GUARD_IOC_REPORT_UNDERRUN,
				"REPORT_UNDERRUN",
				&stats->underrun_reports) < 0)
			return -1;
	}

	if (!cfg->enable_recover) {
		res_guard_alsa_demo_report_event(guard_fd,
			RES_GUARD_IOC_REPORT_RECOVERY_FAIL,
			"REPORT_RECOVERY_FAIL",
			&stats->recovery_fail_reports);
		fprintf(stderr, "recovery disabled, stop on write error: %s\n",
			snd_strerror((int)write_ret));
		return -1;
	}

	err = snd_pcm_recover(pcm, (int)write_ret, 1);
	if (err >= 0) {
		if (res_guard_alsa_demo_report_event(guard_fd,
				RES_GUARD_IOC_REPORT_RECOVERY_OK,
				"REPORT_RECOVERY_OK",
				&stats->recovery_ok_reports) < 0)
			return -1;
		res_guard_alsa_demo_log(cfg, "INFO",
			"snd_pcm_recover success after write error: %s",
			snd_strerror((int)write_ret));
		return 0;
	}

	err = snd_pcm_prepare(pcm);
	if (err >= 0) {
		if (res_guard_alsa_demo_report_event(guard_fd,
				RES_GUARD_IOC_REPORT_RECOVERY_OK,
				"REPORT_RECOVERY_OK",
				&stats->recovery_ok_reports) < 0)
			return -1;
		res_guard_alsa_demo_log(cfg, "INFO",
			"snd_pcm_prepare fallback success after recover failure");
		return 0;
	}

	if (res_guard_alsa_demo_report_event(guard_fd,
			RES_GUARD_IOC_REPORT_RECOVERY_FAIL,
			"REPORT_RECOVERY_FAIL",
			&stats->recovery_fail_reports) < 0)
		return -1;

	fprintf(stderr,
		"snd_pcm_recover/prepare failed after write error %s: %s\n",
		snd_strerror((int)write_ret),
		snd_strerror(err));
	return -1;
}

static int res_guard_alsa_demo_playback_loop(snd_pcm_t *pcm,
	const struct res_guard_alsa_demo_config *cfg,
	int guard_fd,
	struct res_guard_alsa_demo_stats *stats)
{
	int16_t *buffer;
	double phase;
	uint64_t frames_target;
	uint64_t frames_left;

	buffer = calloc(cfg->period_frames * cfg->channels, sizeof(int16_t));
	if (buffer == NULL) {
		fprintf(stderr, "failed to allocate audio buffer\n");
		return -1;
	}

	phase = 0.0;
	frames_target = (uint64_t)cfg->rate * (uint64_t)cfg->seconds;
	frames_left = frames_target;

	while (frames_left > 0) {
		snd_pcm_uframes_t chunk_frames;
		snd_pcm_uframes_t frame_offset;

		if (cfg->stall_every > 0 &&
			stats->write_calls > 0 &&
			stats->write_calls % cfg->stall_every == 0 &&
			cfg->stall_ms > 0) {
			res_guard_alsa_demo_log(cfg, "INFO",
				"stalling %u ms before write call %u",
				cfg->stall_ms,
				stats->write_calls + 1);
			usleep(cfg->stall_ms * 1000U);
		}

		chunk_frames = cfg->period_frames;
		if ((uint64_t)chunk_frames > frames_left)
			chunk_frames = (snd_pcm_uframes_t)frames_left;

		res_guard_alsa_demo_fill_tone(buffer,
			chunk_frames,
			cfg->channels,
			cfg->rate,
			cfg->tone_hz,
			cfg->volume,
			&phase);

		frame_offset = 0;
		while (frame_offset < chunk_frames) {
			snd_pcm_sframes_t written;

			written = snd_pcm_writei(pcm,
				buffer + (frame_offset * cfg->channels),
				chunk_frames - frame_offset);

			if (written > 0) {
				frame_offset += (snd_pcm_uframes_t)written;
				stats->frames_written += (uint64_t)written;
				continue;
			}

			if (written == -EAGAIN)
				continue;

			if (res_guard_alsa_demo_recover(pcm,
					written,
					cfg,
					guard_fd,
					stats) < 0) {
				free(buffer);
				return -1;
			}
		}

		frames_left -= chunk_frames;
		stats->write_calls++;
	}

	free(buffer);
	return 0;
}

int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"pcm-device", required_argument, NULL, 'D'},
		{"guard-device", required_argument, NULL, 'g'},
		{"rate", required_argument, NULL, 'r'},
		{"channels", required_argument, NULL, 'c'},
		{"period", required_argument, NULL, 'p'},
		{"buffer", required_argument, NULL, 'b'},
		{"seconds", required_argument, NULL, 's'},
		{"tone-hz", required_argument, NULL, 'f'},
		{"volume", required_argument, NULL, 'v'},
		{"stall-every", required_argument, NULL, 1000},
		{"stall-ms", required_argument, NULL, 1001},
		{"no-guard", no_argument, NULL, 1002},
		{"no-recover", no_argument, NULL, 1003},
		{"quiet", no_argument, NULL, 'q'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};
	struct res_guard_alsa_demo_config cfg;
	struct res_guard_alsa_demo_stats stats;
	snd_pcm_t *pcm;
	int guard_fd;
	int opt;
	int err;
	int ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.pcm_device = RES_GUARD_ALSA_DEFAULT_PCM_DEVICE;
	cfg.guard_device = RES_GUARD_ALSA_DEFAULT_GUARD_DEVICE;
	cfg.rate = RES_GUARD_ALSA_DEFAULT_RATE;
	cfg.channels = RES_GUARD_ALSA_DEFAULT_CHANNELS;
	cfg.period_frames = RES_GUARD_ALSA_DEFAULT_PERIOD_FRAMES;
	cfg.buffer_frames = RES_GUARD_ALSA_DEFAULT_BUFFER_FRAMES;
	cfg.seconds = RES_GUARD_ALSA_DEFAULT_SECONDS;
	cfg.tone_hz = RES_GUARD_ALSA_DEFAULT_TONE_HZ;
	cfg.volume = RES_GUARD_ALSA_DEFAULT_VOLUME;
	cfg.use_guard = true;
	cfg.enable_recover = true;
	cfg.verbose = true;

	memset(&stats, 0, sizeof(stats));
	pcm = NULL;
	guard_fd = -1;

	while ((opt = getopt_long(argc, argv, "D:g:r:c:p:b:s:f:v:qh", long_options, NULL)) != -1) {
		unsigned int parsed_uint;
		double parsed_double;

		switch (opt) {
		case 'D':
			cfg.pcm_device = optarg;
			break;
		case 'g':
			cfg.guard_device = optarg;
			break;
		case 'r':
			if (res_guard_alsa_demo_parse_uint(optarg, &cfg.rate) < 0) {
				fprintf(stderr, "invalid rate: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'c':
			if (res_guard_alsa_demo_parse_uint(optarg, &cfg.channels) < 0 ||
				cfg.channels == 0) {
				fprintf(stderr, "invalid channels: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'p':
			if (res_guard_alsa_demo_parse_uint(optarg, &parsed_uint) < 0 ||
				parsed_uint == 0) {
				fprintf(stderr, "invalid period: %s\n", optarg);
				return EXIT_FAILURE;
			}
			cfg.period_frames = parsed_uint;
			break;
		case 'b':
			if (res_guard_alsa_demo_parse_uint(optarg, &parsed_uint) < 0 ||
				parsed_uint == 0) {
				fprintf(stderr, "invalid buffer: %s\n", optarg);
				return EXIT_FAILURE;
			}
			cfg.buffer_frames = parsed_uint;
			break;
		case 's':
			if (res_guard_alsa_demo_parse_uint(optarg, &cfg.seconds) < 0 ||
				cfg.seconds == 0) {
				fprintf(stderr, "invalid seconds: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'f':
			if (res_guard_alsa_demo_parse_double(optarg, &cfg.tone_hz) < 0 ||
				cfg.tone_hz <= 0.0) {
				fprintf(stderr, "invalid tone-hz: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'v':
			if (res_guard_alsa_demo_parse_double(optarg, &parsed_double) < 0 ||
				parsed_double < 0.0 ||
				parsed_double > 1.0) {
				fprintf(stderr, "invalid volume: %s\n", optarg);
				return EXIT_FAILURE;
			}
			cfg.volume = parsed_double;
			break;
		case 'q':
			cfg.verbose = false;
			break;
		case 'h':
			res_guard_alsa_demo_usage(argv[0]);
			return EXIT_SUCCESS;
		case 1000:
			if (res_guard_alsa_demo_parse_uint(optarg, &cfg.stall_every) < 0 ||
				cfg.stall_every == 0) {
				fprintf(stderr, "invalid stall-every: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 1001:
			if (res_guard_alsa_demo_parse_uint(optarg, &cfg.stall_ms) < 0) {
				fprintf(stderr, "invalid stall-ms: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 1002:
			cfg.use_guard = false;
			break;
		case 1003:
			cfg.enable_recover = false;
			break;
		default:
			res_guard_alsa_demo_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	res_guard_alsa_demo_log(&cfg, "INFO",
		"pcm_device=%s guard_device=%s rate=%u channels=%u period=%u buffer=%u seconds=%u tone_hz=%.1f volume=%.2f stall_every=%u stall_ms=%u use_guard=%s recover=%s",
		cfg.pcm_device,
		cfg.guard_device,
		cfg.rate,
		cfg.channels,
		(unsigned int)cfg.period_frames,
		(unsigned int)cfg.buffer_frames,
		cfg.seconds,
		cfg.tone_hz,
		cfg.volume,
		cfg.stall_every,
		cfg.stall_ms,
		cfg.use_guard ? "true" : "false",
		cfg.enable_recover ? "true" : "false");

	guard_fd = res_guard_alsa_demo_open_guard(&cfg);
	if (cfg.use_guard && guard_fd < 0)
		return EXIT_FAILURE;

	err = snd_pcm_open(&pcm, cfg.pcm_device, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "snd_pcm_open(%s) failed: %s\n",
			cfg.pcm_device,
			snd_strerror(err));
		ret = EXIT_FAILURE;
		goto out;
	}

	if (res_guard_alsa_demo_configure_pcm(pcm, &cfg) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	res_guard_alsa_demo_log(&cfg, "INFO",
		"actual playback config rate=%u channels=%u period=%u buffer=%u",
		cfg.rate,
		cfg.channels,
		(unsigned int)cfg.period_frames,
		(unsigned int)cfg.buffer_frames);

	if (res_guard_alsa_demo_playback_loop(pcm, &cfg, guard_fd, &stats) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	ret = EXIT_SUCCESS;

out:
	if (pcm != NULL) {
		snd_pcm_drain(pcm);
		snd_pcm_close(pcm);
	}

	if (guard_fd >= 0)
		close(guard_fd);

	fprintf(stdout,
		"summary: writes=%u frames_written=%llu underrun_reports=%u recovery_ok_reports=%u recovery_fail_reports=%u\n",
		stats.write_calls,
		(unsigned long long)stats.frames_written,
		stats.underrun_reports,
		stats.recovery_ok_reports,
		stats.recovery_fail_reports);

	return ret;
}
