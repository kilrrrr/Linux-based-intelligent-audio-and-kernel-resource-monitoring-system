#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <unistd.h>
#include "camera.h"
#include "device.h"
#include "link.h"
#include "main.h"
#include "player.h"
#include "socket.h"

int g_buttonfd = -1;
int g_ledfd = -1;
int g_mixerfd = -1;
int g_sockfd = -1;
struct Node *head = NULL;
extern int shmid;
extern void *g_addr;
fd_set readfd, tmpfd;
int g_maxfd = 0;

void InitSelect();
void m_select();

static void handler(int sig)
{
	shm s;

	(void)sig;
	memset(&s, 0, sizeof(s));

	if (g_addr != NULL)
	{
		memcpy(&s, g_addr, sizeof(s));
		if (s.child_pid > 0)
		{
			kill(s.child_pid, SIGKILL);
		}
		if (s.grand_pid > 0)
		{
			kill(s.grand_pid, SIGKILL);
		}

		shmdt(g_addr);
		shmctl(shmid, IPC_RMID, NULL);
	}

	if (g_sockfd >= 0)
	{
		close(g_sockfd);
	}

	camera_stop();

	exit(0);
}

int main()
{
	int ret;

	ret = InitDriver();
	if (ret == FAILURE)
	{
		printf("InitDriver failed\n");
	}

	InitSelect();

	ret = InitCamera();
	if (ret == FAILURE)
	{
		printf("InitCamera failed\n");
	}

	ret = InitSocket();
	if (ret == FAILURE)
	{
		printf("InitSocket failed\n");
	}

	ret = InitLink();
	if (ret == FAILURE)
	{
		printf("InitLink failed\n");
		exit(1);
	}

	if (access(MUSICPATH, R_OK) == 0)
	{
		GetMusic();
	}

	signal(SIGINT, handler);

	m_select();

	return 0;
}
