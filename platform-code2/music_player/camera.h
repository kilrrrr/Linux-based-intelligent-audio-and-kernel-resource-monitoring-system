#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

#define CAMERA_DEVICE_PATH "/dev/video0"
#define CAMERA_SYSFS_VIDEO_PATH "/sys/class/video4linux"
#define CAMERA_CAPTURE_DIR "/tmp/music_player_camera"
#define CAMERA_PATH_LEN 256
#define CAMERA_MESSAGE_LEN 128
#define CAMERA_DEVICE_NODE_LEN 64
#define CAMERA_DEVICE_NAME_LEN 64
#define CAMERA_PIXEL_FORMAT_LEN 16
#define CAMERA_WIDTH 1920
#define CAMERA_HEIGHT 1080
#define CAMERA_BUFFER_COUNT 4

struct camera_status
{
	int available;
	int running;
	int capture_count;
	int width;
	int height;
	int multiplanar;
	int plane_count;
	char last_file[CAMERA_PATH_LEN];
	char last_error[CAMERA_MESSAGE_LEN];
	char device_node[CAMERA_DEVICE_NODE_LEN];
	char device_name[CAMERA_DEVICE_NAME_LEN];
	char pixel_format[CAMERA_PIXEL_FORMAT_LEN];
};
typedef struct camera_status CameraStatus;

int InitCamera();
int camera_start();
int camera_stop();
int camera_capture(char *path, size_t size);
void camera_get_status(CameraStatus *status);

#endif
