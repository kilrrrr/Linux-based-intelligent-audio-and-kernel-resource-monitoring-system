#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "cJSON.h"
#include "camera.h"
#include "device.h"
#include "link.h"
#include "main.h"
#include "player.h"
#include "socket.h"

extern Node *head;
extern int g_sockfd;
extern int iLeft;
extern int g_start_flag;
extern int g_suspend_flag;
extern fd_set readfd;
extern int g_maxfd;
extern void *g_addr;

static void send_cjson_object(cJSON *object)
{
	char *buf;
	char *payload;
	size_t len;
	int ret;

	if (NULL == object)
	{
		return;
	}

	buf = cJSON_PrintUnformatted(object);
	if (NULL == buf)
	{
		cJSON_Delete(object);
		return;
	}

	len = strlen(buf);
	payload = (char *)malloc(len + 2);
	if (NULL == payload)
	{
		free(buf);
		cJSON_Delete(object);
		return;
	}

	memcpy(payload, buf, len);
	payload[len] = '\n';
	payload[len + 1] = '\0';

	ret = send(g_sockfd, payload, len + 1, 0);
	if (-1 == ret)
	{
		perror("send");
	}

	free(payload);
	free(buf);
	cJSON_Delete(object);
}

static void send_simple_reply(const char *result)
{
	cJSON *cjson_object = cJSON_CreateObject();

	cJSON_AddStringToObject(cjson_object, "cmd", "reply");
	cJSON_AddStringToObject(cjson_object, "result", result);
	send_cjson_object(cjson_object);
}

static const char *camera_state_to_text(const CameraStatus *status)
{
	if (NULL == status)
	{
		return "unknown";
	}

	if (status->available == 0)
	{
		return "unavailable";
	}

	if (status->running == 0)
	{
		return "stopped";
	}

	return "running";
}

static void send_camera_reply(const char *action, int result, const char *message, const char *file_path)
{
	CameraStatus status;
	cJSON *cjson_object = cJSON_CreateObject();
	const char *final_message = message;
	const char *final_file = file_path;

	camera_get_status(&status);

	if ((NULL == final_message || final_message[0] == '\0') && status.last_error[0] != '\0')
	{
		final_message = status.last_error;
	}
	if (NULL == final_message || final_message[0] == '\0')
	{
		final_message = "ok";
	}

	if (NULL == final_file || final_file[0] == '\0')
	{
		final_file = status.last_file;
	}

	cJSON_AddStringToObject(cjson_object, "cmd", "reply_camera");
	cJSON_AddStringToObject(cjson_object, "action", action);
	cJSON_AddStringToObject(cjson_object, "result", (result == SUCCESS) ? "success" : "failure");
	cJSON_AddStringToObject(cjson_object, "camera_status", camera_state_to_text(&status));
	cJSON_AddNumberToObject(cjson_object, "available", status.available);
	cJSON_AddNumberToObject(cjson_object, "running", status.running);
	cJSON_AddNumberToObject(cjson_object, "capture_count", status.capture_count);
	cJSON_AddNumberToObject(cjson_object, "width", status.width);
	cJSON_AddNumberToObject(cjson_object, "height", status.height);
	cJSON_AddNumberToObject(cjson_object, "multiplanar", status.multiplanar);
	cJSON_AddNumberToObject(cjson_object, "plane_count", status.plane_count);
	cJSON_AddStringToObject(cjson_object, "device_node", status.device_node);
	cJSON_AddStringToObject(cjson_object, "device_name", status.device_name);
	cJSON_AddStringToObject(cjson_object, "pixel_format", status.pixel_format);
	cJSON_AddStringToObject(cjson_object, "file", final_file);
	cJSON_AddStringToObject(cjson_object, "message", final_message);
	send_cjson_object(cjson_object);
}

static void send_server(int sig)
{
	cJSON *cjson_sendserver = cJSON_CreateObject();

	(void)sig;
	cJSON_AddStringToObject(cjson_sendserver, "cmd", "info");
	cJSON_AddStringToObject(cjson_sendserver, "status", "alive");
	cJSON_AddStringToObject(cjson_sendserver, "deviceid", "001");
	send_cjson_object(cjson_sendserver);

	alarm(TIMEOUT);
}

static void *connect_cb(void *arg)
{
	int count = 5;
	int ret;
	struct sockaddr_in server_addr;

	(void)arg;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = PF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

	while (count-- > 0)
	{
		ret = connect(g_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
		if (-1 == ret)
		{
			sleep(5);
			continue;
		}

		signal(SIGALRM, send_server);
		alarm(TIMEOUT);
		return NULL;
	}

	return NULL;
}

int InitSocket()
{
	pthread_t tid;
	int ret;

	g_sockfd = socket(PF_INET, SOCK_STREAM, 0);
	if (-1 == g_sockfd)
	{
		return FAILURE;
	}

	FD_SET(g_sockfd, &readfd);
	if (g_sockfd > g_maxfd)
	{
		g_maxfd = g_sockfd;
	}

	ret = pthread_create(&tid, NULL, connect_cb, NULL);
	if (ret != 0)
	{
		return FAILURE;
	}

	return SUCCESS;
}

void socket_start_play()
{
	send_simple_reply("start_success");
}

void socket_stop_play()
{
	send_simple_reply("stop_success");
}

void socket_suspend_play()
{
	send_simple_reply("suspend_success");
}

void socket_continue_play()
{
	send_simple_reply("continue_success");
}

void socket_prior_play()
{
	send_simple_reply("success");
}

void socket_next_play()
{
	send_simple_reply("success");
}

void socket_voice_up_play()
{
	send_simple_reply("success");
}

void socket_voice_down_play()
{
	send_simple_reply("success");
}

void socket_mode_play(int mode)
{
	(void)mode;
	send_simple_reply("success");
}

void socket_get_status()
{
	cJSON *cjson_object = cJSON_CreateObject();
	shm s;
	char music_name[64] = {0};

	cJSON_AddStringToObject(cjson_object, "cmd", "reply_status");

	if (g_start_flag == 1 && g_suspend_flag == 0)
	{
		cJSON_AddStringToObject(cjson_object, "status", "start");
	}
	else if (g_start_flag == 1 && g_suspend_flag == 1)
	{
		cJSON_AddStringToObject(cjson_object, "status", "suspend");
	}
	else
	{
		cJSON_AddStringToObject(cjson_object, "status", "stop");
	}

	cJSON_AddNumberToObject(cjson_object, "voice", iLeft);

	if (g_addr != NULL)
	{
		memset(&s, 0, sizeof(s));
		memcpy(&s, g_addr, sizeof(s));
		snprintf(music_name, sizeof(music_name), "%s", s.cur_name);
	}

	cJSON_AddStringToObject(cjson_object, "music", music_name);
	send_cjson_object(cjson_object);
}

void socket_get_music()
{
	cJSON *cjson_object = cJSON_CreateObject();
	cJSON *array = cJSON_CreateArray();
	Node *p;

	cJSON_AddStringToObject(cjson_object, "cmd", "reply_music");

	if (NULL != head)
	{
		p = head->next;
		while (p != NULL && p != head)
		{
			cJSON_AddItemToArray(array, cJSON_CreateString(p->music_name));
			p = p->next;
		}
	}

	cJSON_AddItemToObject(cjson_object, "music", array);
	send_cjson_object(cjson_object);
}

void socket_camera_start()
{
	int ret = camera_start();
	send_camera_reply("start", ret, NULL, NULL);
}

void socket_camera_stop()
{
	int ret = camera_stop();
	send_camera_reply("stop", ret, NULL, NULL);
}

void socket_camera_capture()
{
	char file_path[CAMERA_PATH_LEN] = {0};
	int ret = camera_capture(file_path, sizeof(file_path));
	send_camera_reply("capture", ret, NULL, file_path);
}

void socket_get_camera_status()
{
	send_camera_reply("status", SUCCESS, NULL, NULL);
}
