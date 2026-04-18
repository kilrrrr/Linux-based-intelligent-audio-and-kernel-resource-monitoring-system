#ifndef PLAYER_H
#define PLAYER_H

#include <list>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <json-c/json.h>
#include "node.h"

class Player
{
public:
	void player_alive_info(std::list<Node> *l, struct bufferevent *bev, struct json_object *val, struct event_base *base);
	void player_operation(std::list<Node> *l, struct bufferevent *app_bev, const char *cmd);
	void player_reply_result(std::list<Node> *l, struct bufferevent *bev, struct json_object *val);

	static void timeout_cb(evutil_socket_t fd, short event, void *arg);
};

#endif
