#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "cJSON.h"
#include "device.h"
#include "player.h"
#include "socket.h"

extern int g_buttonfd;
extern int g_sockfd;
extern fd_set readfd;
extern fd_set tmpfd;
extern int g_maxfd;

static int extract_line(char *buffer, int *length, char *line, size_t line_size)
{
	int i;
	int line_len;

	if (NULL == buffer || NULL == length || NULL == line || line_size == 0)
	{
		return 0;
	}

	for (i = 0; i < *length; ++i)
	{
		if (buffer[i] == '\n')
		{
			line_len = i;
			if ((size_t)line_len >= line_size)
			{
				line_len = (int)line_size - 1;
			}

			memcpy(line, buffer, line_len);
			line[line_len] = '\0';
			memmove(buffer, buffer + i + 1, *length - i - 1);
			*length -= (i + 1);
			return 1;
		}
	}

	return 0;
}

static void show()
{
	printf("Music commands: start stop suspend continue prior next voice mode music\n");
	printf("Camera commands: camera_start camera_stop camera_capture camera_status\n");
}

static void handle_socket_command(const char *cmd)
{
	if (!strcmp(cmd, "start"))
	{
		socket_start_play();
	}
	else if (!strcmp(cmd, "stop"))
	{
		socket_stop_play();
	}
	else if (!strcmp(cmd, "suspend"))
	{
		socket_suspend_play();
	}
	else if (!strcmp(cmd, "continue"))
	{
		socket_continue_play();
	}
	else if (!strcmp(cmd, "prior"))
	{
		socket_prior_play();
	}
	else if (!strcmp(cmd, "next"))
	{
		socket_next_play();
	}
	else if (!strcmp(cmd, "voice_up"))
	{
		socket_voice_up_play();
	}
	else if (!strcmp(cmd, "voice_down"))
	{
		socket_voice_down_play();
	}
	else if (!strcmp(cmd, "sequence"))
	{
		socket_mode_play(SEQUENCEMODE);
	}
	else if (!strcmp(cmd, "random"))
	{
		socket_mode_play(RANDOM);
	}
	else if (!strcmp(cmd, "circle"))
	{
		socket_mode_play(CIRCLE);
	}
	else if (!strcmp(cmd, "get"))
	{
		socket_get_status();
	}
	else if (!strcmp(cmd, "music"))
	{
		socket_get_music();
	}
	else if (!strcmp(cmd, "camera_start"))
	{
		socket_camera_start();
	}
	else if (!strcmp(cmd, "camera_stop"))
	{
		socket_camera_stop();
	}
	else if (!strcmp(cmd, "camera_capture"))
	{
		socket_camera_capture();
	}
	else if (!strcmp(cmd, "camera_status"))
	{
		socket_get_camera_status();
	}
}

void InitSelect()
{
	FD_ZERO(&readfd);
	FD_ZERO(&tmpfd);
	g_maxfd = 0;

	if (g_buttonfd >= 0)
	{
		FD_SET(g_buttonfd, &readfd);
		g_maxfd = g_buttonfd;
	}
}

void m_select()
{
	int ret;
	char message[1024];
	char pending[4096] = {0};
	char line[1024];
	int pending_len = 0;
	cJSON *cjson_object;
	cJSON *cmd_item;

	show();

	while (1)
	{
		tmpfd = readfd;
		ret = select(g_maxfd + 1, &tmpfd, NULL, NULL, NULL);
		if (-1 == ret)
		{
			if (errno == EINTR)
			{
				continue;
			}

			perror("select");
			continue;
		}

		if (g_sockfd >= 0 && FD_ISSET(g_sockfd, &tmpfd))
		{
			memset(message, 0, sizeof(message));
			ret = recv(g_sockfd, message, sizeof(message) - 1, 0);
			if (ret < 0)
			{
				perror("recv");
			}
			else if (0 == ret)
			{
				close(g_sockfd);
				FD_CLR(g_sockfd, &readfd);
				g_sockfd = -1;
			}
			else
			{
				if (pending_len + ret >= (int)sizeof(pending))
				{
					pending_len = 0;
				}

				memcpy(pending + pending_len, message, ret);
				pending_len += ret;
				pending[pending_len] = '\0';

				while (extract_line(pending, &pending_len, line, sizeof(line)))
				{
					cjson_object = cJSON_Parse(line);
					if (NULL == cjson_object)
					{
						printf("invalid json: %s\n", line);
						continue;
					}

					cmd_item = cJSON_GetObjectItem(cjson_object, "cmd");
					if (NULL != cmd_item && NULL != cmd_item->valuestring)
					{
						handle_socket_command(cmd_item->valuestring);
					}

					cJSON_Delete(cjson_object);
				}
			}
		}

		if (g_buttonfd >= 0 && FD_ISSET(g_buttonfd, &tmpfd))
		{
			int id = get_key_id();

			switch (id)
			{
				case 1:
					start_play();
					break;
				case 2:
					stop_play();
					break;
				case 3:
					suspend_play();
					break;
				case 4:
					continue_play();
					break;
				case 5:
					prior_play();
					break;
				case 6:
					next_play();
					break;
			}
		}
	}
}
