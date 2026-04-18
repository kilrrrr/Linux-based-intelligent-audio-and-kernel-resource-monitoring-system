#include "server.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <event2/buffer.h>
#include <iostream>
#include <json-c/json.h>
#include <string>

namespace
{
const char *json_string(struct json_object *obj, const char *key)
{
	struct json_object *value = NULL;

	if (NULL == obj || NULL == key)
	{
		return "";
	}

	if (!json_object_object_get_ex(obj, key, &value) || NULL == value)
	{
		return "";
	}

	return json_object_get_string(value);
}

void write_json(struct bufferevent *bev, struct json_object *obj)
{
	const char *text;
	std::string payload;

	if (NULL == bev || NULL == obj)
	{
		if (NULL != obj)
		{
			json_object_put(obj);
		}
		return;
	}

	text = json_object_to_json_string(obj);
	if (NULL != text)
	{
		payload = text;
		payload.push_back('\n');
		bufferevent_write(bev, payload.c_str(), payload.size());
	}

	json_object_put(obj);
}

bool is_app_operation(const char *cmd)
{
	return !std::strcmp(cmd, "app_start") ||
		!std::strcmp(cmd, "app_stop") ||
		!std::strcmp(cmd, "app_suspend") ||
		!std::strcmp(cmd, "app_continue") ||
		!std::strcmp(cmd, "app_prior") ||
		!std::strcmp(cmd, "app_next") ||
		!std::strcmp(cmd, "app_voice_up") ||
		!std::strcmp(cmd, "app_voice_down") ||
		!std::strcmp(cmd, "app_sequence") ||
		!std::strcmp(cmd, "app_random") ||
		!std::strcmp(cmd, "app_circle") ||
		!std::strcmp(cmd, "app_music") ||
		!std::strcmp(cmd, "app_camera_start") ||
		!std::strcmp(cmd, "app_camera_stop") ||
		!std::strcmp(cmd, "app_camera_capture") ||
		!std::strcmp(cmd, "app_camera_status");
}

Node *find_by_app_id(std::list<Node> *nodes, const char *app_id)
{
	for (std::list<Node>::iterator it = nodes->begin(); it != nodes->end(); ++it)
	{
		if (std::strcmp(it->app_id, app_id) == 0)
		{
			return &(*it);
		}
	}

	return NULL;
}
}

Player *PlayerServer::p = new Player();
std::list<Node> *PlayerServer::l = new std::list<Node>();

PlayerServer::PlayerServer(const char *ip, int port)
{
	struct sockaddr_in server_addr;

	base = event_base_new();
	std::memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = inet_addr(ip);

	listener = evconnlistener_new_bind(base,
		listener_cb,
		base,
		LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
		10,
		(struct sockaddr *)&server_addr,
		sizeof(server_addr));

	if (NULL == listener)
	{
		std::cout << "evconnlistener_new_bind error" << std::endl;
		return;
	}

	event_base_dispatch(base);
}

PlayerServer::~PlayerServer()
{
	if (NULL != listener)
	{
		evconnlistener_free(listener);
	}
	if (NULL != base)
	{
		event_base_free(base);
	}
}

void PlayerServer::listener_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *addr, int socklen, void *arg)
{
	struct event_base *base = static_cast<struct event_base *>(arg);
	struct bufferevent *bev;

	(void)listener;
	(void)addr;
	(void)socklen;

	bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
	if (NULL == bev)
	{
		std::cout << "bufferevent_socket_new error" << std::endl;
		return;
	}

	bufferevent_setcb(bev, read_cb, NULL, event_cb, base);
	bufferevent_enable(bev, EV_READ | EV_WRITE);
}

void PlayerServer::read_cb(struct bufferevent *bev, void *ctx)
{
	struct event_base *base = static_cast<struct event_base *>(ctx);
	struct evbuffer *input = bufferevent_get_input(bev);
	char *line = NULL;
	size_t line_len = 0;
	struct json_object *val;
	const char *cmd;

	if (NULL == input)
	{
		return;
	}

	while ((line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_LF)) != NULL)
	{
		val = json_tokener_parse(line);
		if (NULL == val)
		{
			std::cout << "invalid json: " << line << std::endl;
			free(line);
			continue;
		}

		cmd = json_string(val, "cmd");

		if (!std::strcmp(cmd, "bind"))
		{
			const char *app_id = json_string(val, "appid");
			const char *device_id = json_string(val, "deviceid");
			Node *node = find_by_app_id(l, app_id);
			struct json_object *reply = json_object_new_object();

			if (NULL == node)
			{
				Node n = {};
				n.app_bev = bev;
				std::snprintf(n.app_id, sizeof(n.app_id), "%s", app_id);
				std::snprintf(n.device_id, sizeof(n.device_id), "%s", device_id);
				n.online_flag = 0;
				n.app_online_flag = 1;
				l->push_back(n);
			}
			else
			{
				node->app_bev = bev;
				node->app_online_flag = 1;
				std::snprintf(node->device_id, sizeof(node->device_id), "%s", device_id);
			}

			json_object_object_add(reply, "cmd", json_object_new_string("bind_success"));
			write_json(bev, reply);
		}
		else if (!std::strcmp(cmd, "search_bind"))
		{
			const char *app_id = json_string(val, "appid");
			Node *node = find_by_app_id(l, app_id);
			struct json_object *reply = json_object_new_object();

			json_object_object_add(reply, "cmd", json_object_new_string("reply_bind"));

			if (NULL != node)
			{
				node->app_bev = bev;
				node->app_online_flag = 1;
				json_object_object_add(reply, "result", json_object_new_string("yes"));
			}
			else
			{
				json_object_object_add(reply, "result", json_object_new_string("no"));
			}

			write_json(bev, reply);
		}
		else if (is_app_operation(cmd))
		{
			p->player_operation(l, bev, cmd);
		}
		else if (!std::strcmp(cmd, "app_off_line"))
		{
			for (std::list<Node>::iterator it = l->begin(); it != l->end(); ++it)
			{
				if (it->app_bev == bev)
				{
					it->app_online_flag = 0;
					it->app_bev = NULL;
					break;
				}
			}
		}
		else if (!std::strcmp(cmd, "reply") ||
			!std::strcmp(cmd, "reply_status") ||
			!std::strcmp(cmd, "reply_music") ||
			!std::strcmp(cmd, "reply_camera"))
		{
			p->player_reply_result(l, bev, val);
		}
		else if (!std::strcmp(cmd, "info"))
		{
			p->player_alive_info(l, bev, val, base);
		}

		json_object_put(val);
		free(line);
	}
}

void PlayerServer::event_cb(struct bufferevent *bev, short what, void *ctx)
{
	(void)ctx;

	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
	{
		for (std::list<Node>::iterator it = l->begin(); it != l->end(); ++it)
		{
			if (it->device_bev == bev)
			{
				it->online_flag = 0;
				it->device_bev = NULL;
				event_del(&(it->timeout));
				return;
			}

			if (it->app_bev == bev)
			{
				it->app_online_flag = 0;
				it->app_bev = NULL;
				return;
			}
		}
	}
}
