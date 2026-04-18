#include "camera.h"
#include "main.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct camera_plane
{
	void *start;
	size_t length;
};

struct camera_buffer
{
	unsigned int plane_count;
	struct camera_plane planes[VIDEO_MAX_PLANES];
};

struct camera_runtime
{
	int fd;
	int streaming;
	int multiplanar;
	unsigned int width;
	unsigned int height;
	unsigned int pixel_format;
	unsigned int plane_count;
	unsigned int buffer_count;
	struct camera_buffer buffers[CAMERA_BUFFER_COUNT];
};

static CameraStatus g_camera;
static struct camera_runtime g_runtime = {-1, 0, 0, 0, 0, 0, 0, 0, {{0}}};

static void copy_string(char *dst, size_t size, const char *src)
{
	if (NULL == dst || size == 0)
	{
		return;
	}

	if (NULL == src)
	{
		dst[0] = '\0';
		return;
	}

	snprintf(dst, size, "%s", src);
}

static void set_error(const char *message)
{
	copy_string(g_camera.last_error, sizeof(g_camera.last_error), message);
}

static void set_errno_error(const char *prefix)
{
	char message[CAMERA_MESSAGE_LEN];

	snprintf(message, sizeof(message), "%s: %s", prefix, strerror(errno));
	set_error(message);
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
	{
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static void trim_line(char *text)
{
	size_t len;

	if (NULL == text)
	{
		return;
	}

	len = strlen(text);
	while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
	{
		text[len - 1] = '\0';
		--len;
	}
}

static int read_first_line(const char *path, char *buf, size_t size)
{
	FILE *fp;

	if (NULL == path || NULL == buf || size == 0)
	{
		return FAILURE;
	}

	fp = fopen(path, "r");
	if (NULL == fp)
	{
		return FAILURE;
	}

	if (NULL == fgets(buf, (int)size, fp))
	{
		fclose(fp);
		return FAILURE;
	}

	fclose(fp);
	trim_line(buf);
	return SUCCESS;
}

static int parse_video_index(const char *name)
{
	long value;
	char *endptr;

	if (NULL == name || strncmp(name, "video", 5) != 0)
	{
		return -1;
	}

	if (name[5] == '\0')
	{
		return -1;
	}

	value = strtol(name + 5, &endptr, 10);
	if (*endptr != '\0' || value < 0)
	{
		return -1;
	}

	return (int)value;
}

static int find_video_node_by_name(const char *wanted_name,
	char *device_path,
	size_t device_path_size,
	char *device_name,
	size_t device_name_size)
{
	DIR *dir;
	struct dirent *entry;
	int best_index = -1;
	char best_device[CAMERA_DEVICE_NODE_LEN] = {0};
	char best_name[CAMERA_DEVICE_NAME_LEN] = {0};

	if (NULL == wanted_name || NULL == device_path || NULL == device_name)
	{
		return FAILURE;
	}

	dir = opendir(CAMERA_SYSFS_VIDEO_PATH);
	if (NULL == dir)
	{
		return FAILURE;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		char sysfs_name_path[CAMERA_PATH_LEN];
		char current_name[CAMERA_DEVICE_NAME_LEN];
		int video_index = parse_video_index(entry->d_name);

		if (video_index < 0)
		{
			continue;
		}

		snprintf(sysfs_name_path,
			sizeof(sysfs_name_path),
			"%s/%s/name",
			CAMERA_SYSFS_VIDEO_PATH,
			entry->d_name);
		if (read_first_line(sysfs_name_path, current_name, sizeof(current_name)) == FAILURE)
		{
			continue;
		}

		if (strcmp(current_name, wanted_name) != 0)
		{
			continue;
		}

		if (best_index == -1 || video_index < best_index)
		{
			best_index = video_index;
			snprintf(best_device, sizeof(best_device), "/dev/%s", entry->d_name);
			copy_string(best_name, sizeof(best_name), current_name);
		}
	}

	closedir(dir);

	if (best_index < 0)
	{
		return FAILURE;
	}

	copy_string(device_path, device_path_size, best_device);
	copy_string(device_name, device_name_size, best_name);
	return SUCCESS;
}

static int resolve_camera_device(char *device_path, size_t device_path_size, char *device_name, size_t device_name_size)
{
	char default_name_path[CAMERA_PATH_LEN];

	if (find_video_node_by_name("rkisp_selfpath", device_path, device_path_size, device_name, device_name_size) == SUCCESS)
	{
		return SUCCESS;
	}

	if (find_video_node_by_name("rkisp_mainpath", device_path, device_path_size, device_name, device_name_size) == SUCCESS)
	{
		return SUCCESS;
	}

	if (access(CAMERA_DEVICE_PATH, F_OK) == 0)
	{
		copy_string(device_path, device_path_size, CAMERA_DEVICE_PATH);
		snprintf(default_name_path, sizeof(default_name_path), "%s/video0/name", CAMERA_SYSFS_VIDEO_PATH);
		if (read_first_line(default_name_path, device_name, device_name_size) == FAILURE)
		{
			device_name[0] = '\0';
		}
		return SUCCESS;
	}

	return FAILURE;
}

static void clear_capture_status_fields()
{
	g_camera.width = 0;
	g_camera.height = 0;
	g_camera.multiplanar = 0;
	g_camera.plane_count = 0;
	g_camera.pixel_format[0] = '\0';
}

static void pixel_format_to_text(unsigned int pixel_format, char *buf, size_t size)
{
	unsigned int i;
	char text[5];

	for (i = 0; i < 4; ++i)
	{
		unsigned char value = (unsigned char)((pixel_format >> (i * 8)) & 0xff);
		text[i] = isprint(value) ? (char)value : '.';
	}
	text[4] = '\0';
	copy_string(buf, size, text);
}

static void refresh_camera_available()
{
	char device_path[CAMERA_DEVICE_NODE_LEN] = {0};
	char device_name[CAMERA_DEVICE_NAME_LEN] = {0};

	g_camera.available = 0;
	g_camera.device_node[0] = '\0';
	g_camera.device_name[0] = '\0';

	if (resolve_camera_device(device_path, sizeof(device_path), device_name, sizeof(device_name)) == SUCCESS &&
		access(device_path, F_OK) == 0)
	{
		g_camera.available = 1;
		copy_string(g_camera.device_node, sizeof(g_camera.device_node), device_path);
		copy_string(g_camera.device_name, sizeof(g_camera.device_name), device_name);
	}
}

static int ensure_capture_dir()
{
	int ret = mkdir(CAMERA_CAPTURE_DIR, 0775);

	if (-1 == ret && errno != EEXIST)
	{
		set_error("create capture dir failed");
		return FAILURE;
	}

	return SUCCESS;
}

static const char *capture_extension()
{
	if (g_runtime.pixel_format == V4L2_PIX_FMT_MJPEG)
	{
		return "jpg";
	}

	return "ppm";
}

static int build_capture_path(char *path, size_t size)
{
	time_t now = time(NULL);
	struct tm *tm_now = localtime(&now);
	int ret;

	if (NULL == path || size == 0)
	{
		set_error("invalid capture path buffer");
		return FAILURE;
	}

	if (NULL == tm_now)
	{
		set_error("localtime failed");
		return FAILURE;
	}

	ret = snprintf(path,
		size,
		"%s/capture_%04d%02d%02d_%02d%02d%02d_%03d.%s",
		CAMERA_CAPTURE_DIR,
		tm_now->tm_year + 1900,
		tm_now->tm_mon + 1,
		tm_now->tm_mday,
		tm_now->tm_hour,
		tm_now->tm_min,
		tm_now->tm_sec,
		g_camera.capture_count + 1,
		capture_extension());
	if (ret < 0 || (size_t)ret >= size)
	{
		set_error("capture path too long");
		return FAILURE;
	}

	return SUCCESS;
}

static void reset_runtime()
{
	unsigned int i;
	unsigned int plane_index;
	enum v4l2_buf_type type = g_runtime.multiplanar ?
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
		V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (g_runtime.fd >= 0)
	{
		if (g_runtime.streaming)
		{
			(void)xioctl(g_runtime.fd, VIDIOC_STREAMOFF, &type);
		}

		for (i = 0; i < g_runtime.buffer_count; ++i)
		{
			for (plane_index = 0; plane_index < g_runtime.buffers[i].plane_count; ++plane_index)
			{
				if (g_runtime.buffers[i].planes[plane_index].start != NULL &&
					g_runtime.buffers[i].planes[plane_index].length > 0)
				{
					munmap(g_runtime.buffers[i].planes[plane_index].start,
						g_runtime.buffers[i].planes[plane_index].length);
				}
			}
		}

		close(g_runtime.fd);
	}

	memset(&g_runtime, 0, sizeof(g_runtime));
	g_runtime.fd = -1;
}

static int query_camera_capabilities()
{
	struct v4l2_capability cap;
	unsigned int capabilities;

	memset(&cap, 0, sizeof(cap));
	if (-1 == xioctl(g_runtime.fd, VIDIOC_QUERYCAP, &cap))
	{
		set_errno_error("VIDIOC_QUERYCAP failed");
		return FAILURE;
	}

	capabilities = cap.capabilities;
	if (capabilities & V4L2_CAP_DEVICE_CAPS)
	{
		capabilities = cap.device_caps;
	}

	if ((capabilities & V4L2_CAP_STREAMING) == 0)
	{
		set_error("device does not support streaming io");
		return FAILURE;
	}

	if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
	{
		g_runtime.multiplanar = 1;
		g_runtime.plane_count = 1;
		return SUCCESS;
	}

	if (capabilities & V4L2_CAP_VIDEO_CAPTURE)
	{
		g_runtime.multiplanar = 0;
		g_runtime.plane_count = 1;
		return SUCCESS;
	}

	set_error("device does not support video capture");
	return FAILURE;
}

static int is_supported_pixel_format(unsigned int pixel_format)
{
	return pixel_format == V4L2_PIX_FMT_NV12 ||
		pixel_format == V4L2_PIX_FMT_NV12M ||
		pixel_format == V4L2_PIX_FMT_UYVY ||
		pixel_format == V4L2_PIX_FMT_MJPEG ||
		pixel_format == V4L2_PIX_FMT_YUYV;
}

static int configure_capture_format()
{
	static const unsigned int desired_formats[] = {
		V4L2_PIX_FMT_NV12,
		V4L2_PIX_FMT_NV12M,
		V4L2_PIX_FMT_UYVY,
		V4L2_PIX_FMT_MJPEG,
		V4L2_PIX_FMT_YUYV
	};
	unsigned int i;

	for (i = 0; i < sizeof(desired_formats) / sizeof(desired_formats[0]); ++i)
	{
		struct v4l2_format fmt;
		unsigned int actual_format;
		unsigned int actual_width;
		unsigned int actual_height;
		unsigned int plane_count = 1;

		memset(&fmt, 0, sizeof(fmt));
		if (g_runtime.multiplanar)
		{
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			fmt.fmt.pix_mp.width = CAMERA_WIDTH;
			fmt.fmt.pix_mp.height = CAMERA_HEIGHT;
			fmt.fmt.pix_mp.pixelformat = desired_formats[i];
			fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
			fmt.fmt.pix_mp.num_planes = 1;
		}
		else
		{
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			fmt.fmt.pix.width = CAMERA_WIDTH;
			fmt.fmt.pix.height = CAMERA_HEIGHT;
			fmt.fmt.pix.pixelformat = desired_formats[i];
			fmt.fmt.pix.field = V4L2_FIELD_ANY;
		}

		if (-1 == xioctl(g_runtime.fd, VIDIOC_S_FMT, &fmt))
		{
			continue;
		}

		if (g_runtime.multiplanar)
		{
			actual_format = fmt.fmt.pix_mp.pixelformat;
			actual_width = fmt.fmt.pix_mp.width;
			actual_height = fmt.fmt.pix_mp.height;
			plane_count = fmt.fmt.pix_mp.num_planes;
		}
		else
		{
			actual_format = fmt.fmt.pix.pixelformat;
			actual_width = fmt.fmt.pix.width;
			actual_height = fmt.fmt.pix.height;
		}

		if (plane_count == 0)
		{
			plane_count = 1;
		}

		if (!is_supported_pixel_format(actual_format))
		{
			continue;
		}

		g_runtime.width = actual_width;
		g_runtime.height = actual_height;
		g_runtime.pixel_format = actual_format;
		g_runtime.plane_count = plane_count;
		return SUCCESS;
	}

	set_error("no supported pixel format (NV12/UYVY/MJPEG/YUYV)");
	return FAILURE;
}

static int request_single_plane_buffers()
{
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.count = CAMERA_BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(g_runtime.fd, VIDIOC_REQBUFS, &req))
	{
		set_errno_error("VIDIOC_REQBUFS failed");
		return FAILURE;
	}

	if (req.count < 2)
	{
		set_error("insufficient v4l2 buffers");
		return FAILURE;
	}

	g_runtime.buffer_count = req.count;
	if (g_runtime.buffer_count > CAMERA_BUFFER_COUNT)
	{
		g_runtime.buffer_count = CAMERA_BUFFER_COUNT;
	}

	for (i = 0; i < g_runtime.buffer_count; ++i)
	{
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == xioctl(g_runtime.fd, VIDIOC_QUERYBUF, &buf))
		{
			set_errno_error("VIDIOC_QUERYBUF failed");
			return FAILURE;
		}

		g_runtime.buffers[i].plane_count = 1;
		g_runtime.buffers[i].planes[0].length = buf.length;
		g_runtime.buffers[i].planes[0].start = mmap(NULL,
			buf.length,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			g_runtime.fd,
			buf.m.offset);
		if (MAP_FAILED == g_runtime.buffers[i].planes[0].start)
		{
			g_runtime.buffers[i].planes[0].start = NULL;
			set_errno_error("mmap failed");
			return FAILURE;
		}

		if (-1 == xioctl(g_runtime.fd, VIDIOC_QBUF, &buf))
		{
			set_errno_error("VIDIOC_QBUF failed");
			return FAILURE;
		}
	}

	return SUCCESS;
}

static int request_multiplanar_buffers()
{
	struct v4l2_requestbuffers req;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.count = CAMERA_BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(g_runtime.fd, VIDIOC_REQBUFS, &req))
	{
		set_errno_error("VIDIOC_REQBUFS failed");
		return FAILURE;
	}

	if (req.count < 2)
	{
		set_error("insufficient v4l2 buffers");
		return FAILURE;
	}

	g_runtime.buffer_count = req.count;
	if (g_runtime.buffer_count > CAMERA_BUFFER_COUNT)
	{
		g_runtime.buffer_count = CAMERA_BUFFER_COUNT;
	}

	for (i = 0; i < g_runtime.buffer_count; ++i)
	{
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		unsigned int plane_index;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = VIDEO_MAX_PLANES;
		buf.m.planes = planes;

		if (-1 == xioctl(g_runtime.fd, VIDIOC_QUERYBUF, &buf))
		{
			set_errno_error("VIDIOC_QUERYBUF failed");
			return FAILURE;
		}

		if (buf.length == 0 || buf.length > VIDEO_MAX_PLANES)
		{
			set_error("invalid plane count from driver");
			return FAILURE;
		}

		g_runtime.buffers[i].plane_count = buf.length;
		for (plane_index = 0; plane_index < buf.length; ++plane_index)
		{
			g_runtime.buffers[i].planes[plane_index].length = planes[plane_index].length;
			g_runtime.buffers[i].planes[plane_index].start = mmap(NULL,
				planes[plane_index].length,
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				g_runtime.fd,
				planes[plane_index].m.mem_offset);
			if (MAP_FAILED == g_runtime.buffers[i].planes[plane_index].start)
			{
				g_runtime.buffers[i].planes[plane_index].start = NULL;
				set_errno_error("mmap failed");
				return FAILURE;
			}
		}

		memset(planes, 0, sizeof(planes));
		buf.length = g_runtime.buffers[i].plane_count;
		buf.m.planes = planes;
		for (plane_index = 0; plane_index < g_runtime.buffers[i].plane_count; ++plane_index)
		{
			planes[plane_index].length = g_runtime.buffers[i].planes[plane_index].length;
		}

		if (-1 == xioctl(g_runtime.fd, VIDIOC_QBUF, &buf))
		{
			set_errno_error("VIDIOC_QBUF failed");
			return FAILURE;
		}
	}

	return SUCCESS;
}

static int request_mmap_buffers()
{
	if (g_runtime.multiplanar)
	{
		return request_multiplanar_buffers();
	}

	return request_single_plane_buffers();
}

static int start_streaming()
{
	enum v4l2_buf_type type = g_runtime.multiplanar ?
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
		V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (-1 == xioctl(g_runtime.fd, VIDIOC_STREAMON, &type))
	{
		set_errno_error("VIDIOC_STREAMON failed");
		return FAILURE;
	}

	g_runtime.streaming = 1;
	return SUCCESS;
}

static int wait_for_frame_ready()
{
	fd_set fds;
	struct timeval tv;
	int ret;

	while (1)
	{
		FD_ZERO(&fds);
		FD_SET(g_runtime.fd, &fds);
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		ret = select(g_runtime.fd + 1, &fds, NULL, NULL, &tv);
		if (-1 == ret)
		{
			if (errno == EINTR)
			{
				continue;
			}

			set_errno_error("camera select failed");
			return FAILURE;
		}

		if (0 == ret)
		{
			set_error("camera frame wait timeout");
			return FAILURE;
		}

		return SUCCESS;
	}
}

static int write_all(int fd, const void *buf, size_t size)
{
	const char *ptr = (const char *)buf;
	size_t written = 0;
	ssize_t ret;

	while (written < size)
	{
		ret = write(fd, ptr + written, size - written);
		if (ret < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			return FAILURE;
		}
		if (ret == 0)
		{
			return FAILURE;
		}

		written += (size_t)ret;
	}

	return SUCCESS;
}

static int save_mjpeg_frame(const char *path, const void *data, size_t size)
{
	int fd;
	int ret;

	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0664);
	if (fd < 0)
	{
		set_errno_error("open capture file failed");
		return FAILURE;
	}

	ret = write_all(fd, data, size);
	close(fd);

	if (ret == FAILURE)
	{
		set_errno_error("write MJPEG frame failed");
		return FAILURE;
	}

	return SUCCESS;
}

static int open_ppm_file(const char *path)
{
	int fd;
	char header[64];
	int header_len;

	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0664);
	if (fd < 0)
	{
		set_errno_error("open capture file failed");
		return FAILURE;
	}

	header_len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", g_runtime.width, g_runtime.height);
	if (header_len <= 0 || header_len >= (int)sizeof(header))
	{
		close(fd);
		set_error("PPM header build failed");
		return FAILURE;
	}

	if (write_all(fd, header, (size_t)header_len) == FAILURE)
	{
		close(fd);
		set_errno_error("write PPM header failed");
		return FAILURE;
	}

	return fd;
}

static unsigned char clamp_rgb(int value)
{
	if (value < 0)
	{
		return 0;
	}
	if (value > 255)
	{
		return 255;
	}

	return (unsigned char)value;
}

static void yuv_to_rgb(unsigned char y, unsigned char u, unsigned char v,
	unsigned char *r, unsigned char *g, unsigned char *b)
{
	int c = (int)y - 16;
	int d = (int)u - 128;
	int e = (int)v - 128;
	int red;
	int green;
	int blue;

	if (c < 0)
	{
		c = 0;
	}

	red = (298 * c + 409 * e + 128) >> 8;
	green = (298 * c - 100 * d - 208 * e + 128) >> 8;
	blue = (298 * c + 516 * d + 128) >> 8;

	*r = clamp_rgb(red);
	*g = clamp_rgb(green);
	*b = clamp_rgb(blue);
}

static int save_yuyv_frame_as_ppm(const char *path, const unsigned char *data, size_t size)
{
	int fd;
	unsigned int x;
	unsigned int y;
	size_t required_size;

	required_size = (size_t)g_runtime.width * (size_t)g_runtime.height * 2;
	if (size < required_size)
	{
		set_error("YUYV frame size is too small");
		return FAILURE;
	}

	fd = open_ppm_file(path);
	if (fd == FAILURE)
	{
		return FAILURE;
	}

	for (y = 0; y < g_runtime.height; ++y)
	{
		for (x = 0; x + 1 < g_runtime.width; x += 2)
		{
			size_t index = ((size_t)y * g_runtime.width + x) * 2;
			unsigned char rgb[6];

			yuv_to_rgb(data[index], data[index + 1], data[index + 3], &rgb[0], &rgb[1], &rgb[2]);
			yuv_to_rgb(data[index + 2], data[index + 1], data[index + 3], &rgb[3], &rgb[4], &rgb[5]);

			if (write_all(fd, rgb, sizeof(rgb)) == FAILURE)
			{
				close(fd);
				set_errno_error("write PPM data failed");
				return FAILURE;
			}
		}
	}

	close(fd);
	return SUCCESS;
}

static int save_uyvy_frame_as_ppm(const char *path, const unsigned char *data, size_t size)
{
	int fd;
	unsigned int x;
	unsigned int y;
	size_t required_size;

	required_size = (size_t)g_runtime.width * (size_t)g_runtime.height * 2;
	if (size < required_size)
	{
		set_error("UYVY frame size is too small");
		return FAILURE;
	}

	fd = open_ppm_file(path);
	if (fd == FAILURE)
	{
		return FAILURE;
	}

	for (y = 0; y < g_runtime.height; ++y)
	{
		for (x = 0; x + 1 < g_runtime.width; x += 2)
		{
			size_t index = ((size_t)y * g_runtime.width + x) * 2;
			unsigned char rgb[6];

			yuv_to_rgb(data[index + 1], data[index], data[index + 2], &rgb[0], &rgb[1], &rgb[2]);
			yuv_to_rgb(data[index + 3], data[index], data[index + 2], &rgb[3], &rgb[4], &rgb[5]);

			if (write_all(fd, rgb, sizeof(rgb)) == FAILURE)
			{
				close(fd);
				set_errno_error("write PPM data failed");
				return FAILURE;
			}
		}
	}

	close(fd);
	return SUCCESS;
}

static int save_nv12_frame_as_ppm(const char *path,
	const unsigned char *plane0,
	size_t size0,
	const unsigned char *plane1,
	size_t size1)
{
	int fd;
	unsigned int x;
	unsigned int y;
	size_t y_size = (size_t)g_runtime.width * (size_t)g_runtime.height;
	size_t uv_size = y_size / 2;
	const unsigned char *y_plane = plane0;
	const unsigned char *uv_plane;

	if (plane1 != NULL)
	{
		if (size0 < y_size || size1 < uv_size)
		{
			set_error("NV12 frame size is too small");
			return FAILURE;
		}

		uv_plane = plane1;
	}
	else
	{
		if (size0 < y_size + uv_size)
		{
			set_error("NV12 frame size is too small");
			return FAILURE;
		}

		uv_plane = plane0 + y_size;
	}

	fd = open_ppm_file(path);
	if (fd == FAILURE)
	{
		return FAILURE;
	}

	for (y = 0; y < g_runtime.height; ++y)
	{
		for (x = 0; x < g_runtime.width; ++x)
		{
			size_t y_index = (size_t)y * g_runtime.width + x;
			size_t uv_index = ((size_t)(y / 2) * g_runtime.width) + (x & ~1u);
			unsigned char rgb[3];

			yuv_to_rgb(y_plane[y_index], uv_plane[uv_index], uv_plane[uv_index + 1], &rgb[0], &rgb[1], &rgb[2]);
			if (write_all(fd, rgb, sizeof(rgb)) == FAILURE)
			{
				close(fd);
				set_errno_error("write PPM data failed");
				return FAILURE;
			}
		}
	}

	close(fd);
	return SUCCESS;
}

static size_t frame_bytes_used(const struct camera_buffer *buffer,
	const struct v4l2_buffer *buf,
	const struct v4l2_plane *planes,
	unsigned int plane_index)
{
	size_t bytesused;

	if (g_runtime.multiplanar)
	{
		if (plane_index >= buffer->plane_count)
		{
			return 0;
		}

		bytesused = planes[plane_index].bytesused;
		if (bytesused == 0 || bytesused > buffer->planes[plane_index].length)
		{
			bytesused = buffer->planes[plane_index].length;
		}
		return bytesused;
	}

	bytesused = buf->bytesused;
	if (bytesused == 0 || bytesused > buffer->planes[0].length)
	{
		bytesused = buffer->planes[0].length;
	}

	return bytesused;
}

static int save_frame_to_file(const char *path,
	const struct camera_buffer *buffer,
	const struct v4l2_buffer *buf,
	const struct v4l2_plane *planes)
{
	const unsigned char *plane0 = (const unsigned char *)buffer->planes[0].start;
	size_t plane0_size = frame_bytes_used(buffer, buf, planes, 0);

	if (g_runtime.pixel_format == V4L2_PIX_FMT_MJPEG)
	{
		return save_mjpeg_frame(path, plane0, plane0_size);
	}

	if (g_runtime.pixel_format == V4L2_PIX_FMT_YUYV)
	{
		return save_yuyv_frame_as_ppm(path, plane0, plane0_size);
	}

	if (g_runtime.pixel_format == V4L2_PIX_FMT_UYVY)
	{
		return save_uyvy_frame_as_ppm(path, plane0, plane0_size);
	}

	if (g_runtime.pixel_format == V4L2_PIX_FMT_NV12 || g_runtime.pixel_format == V4L2_PIX_FMT_NV12M)
	{
		const unsigned char *plane1 = NULL;
		size_t plane1_size = 0;

		if (buffer->plane_count > 1)
		{
			plane1 = (const unsigned char *)buffer->planes[1].start;
			plane1_size = frame_bytes_used(buffer, buf, planes, 1);
		}

		return save_nv12_frame_as_ppm(path, plane0, plane0_size, plane1, plane1_size);
	}

	set_error("unsupported pixel format");
	return FAILURE;
}

static int dequeue_frame(struct v4l2_buffer *buf, struct v4l2_plane *planes)
{
	int retry;

	for (retry = 0; retry < 5; ++retry)
	{
		if (wait_for_frame_ready() == FAILURE)
		{
			return FAILURE;
		}

		memset(buf, 0, sizeof(*buf));
		buf->memory = V4L2_MEMORY_MMAP;
		if (g_runtime.multiplanar)
		{
			memset(planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
			buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			buf->length = (g_runtime.plane_count > 0) ? g_runtime.plane_count : VIDEO_MAX_PLANES;
			buf->m.planes = planes;
		}
		else
		{
			buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		}

		if (0 == xioctl(g_runtime.fd, VIDIOC_DQBUF, buf))
		{
			if (buf->index >= g_runtime.buffer_count)
			{
				set_error("invalid buffer index");
				return FAILURE;
			}

			return SUCCESS;
		}

		if (errno != EAGAIN)
		{
			set_errno_error("VIDIOC_DQBUF failed");
			return FAILURE;
		}
	}

	set_error("camera dequeue retry exhausted");
	return FAILURE;
}

static int requeue_frame(struct v4l2_buffer *buf, struct v4l2_plane *planes)
{
	if (g_runtime.multiplanar)
	{
		unsigned int plane_index;
		unsigned int plane_count = g_runtime.buffers[buf->index].plane_count;

		buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf->memory = V4L2_MEMORY_MMAP;
		buf->length = plane_count;
		buf->m.planes = planes;
		for (plane_index = 0; plane_index < plane_count; ++plane_index)
		{
			planes[plane_index].length = g_runtime.buffers[buf->index].planes[plane_index].length;
			planes[plane_index].bytesused = 0;
			planes[plane_index].data_offset = 0;
		}
	}
	else
	{
		buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf->memory = V4L2_MEMORY_MMAP;
	}

	if (-1 == xioctl(g_runtime.fd, VIDIOC_QBUF, buf))
	{
		set_errno_error("VIDIOC_QBUF requeue failed");
		return FAILURE;
	}

	return SUCCESS;
}

static void sync_runtime_status()
{
	g_camera.width = (int)g_runtime.width;
	g_camera.height = (int)g_runtime.height;
	g_camera.multiplanar = g_runtime.multiplanar;
	g_camera.plane_count = (int)g_runtime.plane_count;
	pixel_format_to_text(g_runtime.pixel_format, g_camera.pixel_format, sizeof(g_camera.pixel_format));
}

int InitCamera()
{
	memset(&g_camera, 0, sizeof(g_camera));
	reset_runtime();
	clear_capture_status_fields();

	if (ensure_capture_dir() == FAILURE)
	{
		return FAILURE;
	}

	refresh_camera_available();
	if (g_camera.available == 0)
	{
		set_error("camera device not found");
	}
	else
	{
		set_error(NULL);
	}

	return SUCCESS;
}

int camera_start()
{
	const char *device_path;

	refresh_camera_available();
	if (g_camera.available == 0)
	{
		set_error("camera device not found");
		return FAILURE;
	}

	if (g_camera.running == 1 && g_runtime.fd >= 0)
	{
		set_error(NULL);
		return SUCCESS;
	}

	reset_runtime();
	clear_capture_status_fields();
	device_path = (g_camera.device_node[0] != '\0') ? g_camera.device_node : CAMERA_DEVICE_PATH;

	g_runtime.fd = open(device_path, O_RDWR | O_NONBLOCK);
	if (g_runtime.fd < 0)
	{
		set_errno_error("open camera device failed");
		return FAILURE;
	}

	if (query_camera_capabilities() == FAILURE ||
		configure_capture_format() == FAILURE ||
		request_mmap_buffers() == FAILURE ||
		start_streaming() == FAILURE)
	{
		reset_runtime();
		refresh_camera_available();
		clear_capture_status_fields();
		g_camera.running = 0;
		return FAILURE;
	}

	g_camera.running = 1;
	sync_runtime_status();
	set_error(NULL);
	return SUCCESS;
}

int camera_stop()
{
	reset_runtime();
	refresh_camera_available();
	clear_capture_status_fields();
	g_camera.running = 0;
	set_error(NULL);
	return SUCCESS;
}

int camera_capture(char *path, size_t size)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int ret;

	if (NULL == path || size == 0)
	{
		set_error("invalid capture buffer");
		return FAILURE;
	}

	path[0] = '\0';

	refresh_camera_available();
	if (g_camera.available == 0)
	{
		set_error("camera device not found");
		return FAILURE;
	}

	if (g_camera.running == 0 || g_runtime.fd < 0 || g_runtime.streaming == 0)
	{
		set_error("camera not started");
		return FAILURE;
	}

	if (ensure_capture_dir() == FAILURE)
	{
		return FAILURE;
	}

	if (build_capture_path(path, size) == FAILURE)
	{
		return FAILURE;
	}

	if (dequeue_frame(&buf, planes) == FAILURE)
	{
		path[0] = '\0';
		return FAILURE;
	}

	ret = save_frame_to_file(path, &g_runtime.buffers[buf.index], &buf, planes);
	if (requeue_frame(&buf, planes) == FAILURE)
	{
		path[0] = '\0';
		return FAILURE;
	}

	if (ret == FAILURE)
	{
		path[0] = '\0';
		return FAILURE;
	}

	copy_string(g_camera.last_file, sizeof(g_camera.last_file), path);
	g_camera.capture_count++;
	set_error(NULL);
	return SUCCESS;
}

void camera_get_status(CameraStatus *status)
{
	if (NULL == status)
	{
		return;
	}

	memcpy(status, &g_camera, sizeof(*status));
}
