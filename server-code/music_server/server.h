#ifndef SERVER_H
#define SERVER_H

#include <list>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <netinet/in.h>
#include "node.h"
#include "player.h"

#define IP "192.163.1.100"
#define PORT 8000

class PlayerServer
{
private:
	struct event_base *base;
	struct evconnlistener *listener;

	static Player *p;
	static std::list<Node> *l;

public:
	PlayerServer(const char *ip = IP, int port = PORT);
	~PlayerServer();

private:
	static void listener_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *addr, int socklen, void *arg);
	static void read_cb(struct bufferevent *bev, void *ctx);
	static void event_cb(struct bufferevent *bev, short what, void *ctx);
};

#endif
