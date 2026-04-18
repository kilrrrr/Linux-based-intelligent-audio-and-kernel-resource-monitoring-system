#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "main.h"

extern int g_buttonfd;
extern int g_ledfd;
extern int g_mixerfd;
extern int g_maxfd;

int InitDriver()
{
	int i;

	g_buttonfd = open("/dev/buttons", O_RDONLY);
	if (-1 == g_buttonfd)
	{
		return FAILURE;
	}

	if (g_buttonfd > g_maxfd)
	{
		g_maxfd = g_buttonfd;
	}

	g_ledfd = open("/dev/leds", O_WRONLY);
	if (-1 == g_ledfd)
	{
		return FAILURE;
	}

	for (i = 0; i < 4; i++)
	{
		ioctl(g_ledfd, 0, i);
	}

	g_mixerfd = open("/dev/mixer", O_WRONLY);
	if (-1 == g_mixerfd)
	{
		return FAILURE;
	}

	if (g_mixerfd > g_maxfd)
	{
		g_maxfd = g_mixerfd;
	}

	return SUCCESS;
}

void led_on(int which)
{
	ioctl(g_ledfd, 1, which);
}

void led_off(int which)
{
	ioctl(g_ledfd, 0, which);
}

int get_key_id()
{
	static char buttons[6] = {'0', '0', '0', '0', '0', '0'};
	char cur_buttons[6] = {0};
	int ret;
	int i;

	ret = read(g_buttonfd, cur_buttons, sizeof(cur_buttons));
	if (-1 == ret)
	{
		perror("read");
		return 0;
	}

	for (i = 0; i < 6; i++)
	{
		if (buttons[i] != cur_buttons[i])
		{
			memcpy(buttons, cur_buttons, sizeof(buttons));
			return i + 1;
		}
	}

	memcpy(buttons, cur_buttons, sizeof(buttons));
	return 0;
}
