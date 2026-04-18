#ifndef NODE_H
#define NODE_H

#include <list>
#include <time.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

struct Node
{
	struct bufferevent *app_bev;
	struct bufferevent *device_bev;
	char app_id[32];
	char device_id[32];
	int online_flag;
	int app_online_flag;
	time_t time;
	struct event timeout;
};
typedef struct Node Node;

struct timeout_node
{
	std::list<Node> *l;
	char id[32];
};
typedef struct timeout_node tNode;

#endif
