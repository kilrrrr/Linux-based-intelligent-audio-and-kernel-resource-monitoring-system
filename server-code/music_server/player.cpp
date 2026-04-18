#include "player.h"
#include <cstdio>
#include <cstring>
#include <iostream>
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

const char *camera_action_from_cmd(const char *cmd)
{
	if (!strcmp(cmd, "app_camera_start"))
	{
		return "start";
	}
	if (!strcmp(cmd, "app_camera_stop"))
	{
		return "stop";
	}
	if (!strcmp(cmd, "app_camera_capture"))
	{
		return "capture";
	}
	if (!strcmp(cmd, "app_camera_status"))
	{
		return "status";
	}

	return "";
}

const char *device_cmd_from_app_cmd(const char *cmd)
{
	if (!strcmp(cmd, "app_start"))
	{
		return "start";
	}
	if (!strcmp(cmd, "app_stop"))
	{
		return "stop";
	}
	if (!strcmp(cmd, "app_suspend"))
	{
		return "suspend";
	}
	if (!strcmp(cmd, "app_continue"))
	{
		return "continue";
	}
	if (!strcmp(cmd, "app_prior"))
	{
		return "prior";
	}
	if (!strcmp(cmd, "app_next"))
	{
		return "next";
	}
	if (!strcmp(cmd, "app_voice_up"))
	{
		return "voice_up";
	}
	if (!strcmp(cmd, "app_voice_down"))
	{
		return "voice_down";
	}
	if (!strcmp(cmd, "app_sequence"))
	{
		return "sequence";
	}
	if (!strcmp(cmd, "app_random"))
	{
		return "random";
	}
	if (!strcmp(cmd, "app_circle"))
	{
		return "circle";
	}
	if (!strcmp(cmd, "app_music"))
	{
		return "music";
	}
	if (!strcmp(cmd, "app_camera_start"))
	{
		return "camera_start";
	}
	if (!strcmp(cmd, "app_camera_stop"))
	{
		return "camera_stop";
	}
	if (!strcmp(cmd, "app_camera_capture"))
	{
		return "camera_capture";
	}
	if (!strcmp(cmd, "app_camera_status"))
	{
		return "camera_status";
	}

	return NULL;
}

bool is_camera_command(const char *cmd)
{
	return !strcmp(cmd, "app_camera_start") ||
		!strcmp(cmd, "app_camera_stop") ||
		!strcmp(cmd, "app_camera_capture") ||
		!strcmp(cmd, "app_camera_status");
}

void send_offline_reply(struct bufferevent *app_bev, const char *cmd)
{
	struct json_object *reply = json_object_new_object();

	if (is_camera_command(cmd))
	{
		json_object_object_add(reply, "cmd", json_object_new_string("app_reply_camera"));
		json_object_object_add(reply, "action", json_object_new_string(camera_action_from_cmd(cmd)));
		json_object_object_add(reply, "result", json_object_new_string("failure"));
		json_object_object_add(reply, "camera_status", json_object_new_string("unavailable"));
		json_object_object_add(reply, "available", json_object_new_int(0));
		json_object_object_add(reply, "running", json_object_new_int(0));
		json_object_object_add(reply, "capture_count", json_object_new_int(0));
		json_object_object_add(reply, "file", json_object_new_string(""));
		json_object_object_add(reply, "message", json_object_new_string("device offline"));
	}
	else
	{
		json_object_object_add(reply, "cmd", json_object_new_string("app_reply"));
		json_object_object_add(reply, "result", json_object_new_string("off_line"));
	}

	write_json(app_bev, reply);
}

void send_not_bound_reply(struct bufferevent *app_bev, const char *cmd)
{
	struct json_object *reply = json_object_new_object();

	if (is_camera_command(cmd))
	{
		json_object_object_add(reply, "cmd", json_object_new_string("app_reply_camera"));
		json_object_object_add(reply, "action", json_object_new_string(camera_action_from_cmd(cmd)));
		json_object_object_add(reply, "result", json_object_new_string("failure"));
		json_object_object_add(reply, "camera_status", json_object_new_string("unknown"));
		json_object_object_add(reply, "available", json_object_new_int(0));
		json_object_object_add(reply, "running", json_object_new_int(0));
		json_object_object_add(reply, "capture_count", json_object_new_int(0));
		json_object_object_add(reply, "file", json_object_new_string(""));
		json_object_object_add(reply, "message", json_object_new_string("app not bound"));
	}
	else
	{
		json_object_object_add(reply, "cmd", json_object_new_string("app_reply"));
		json_object_object_add(reply, "result", json_object_new_string("not_bound"));
	}

	write_json(app_bev, reply);
}
}

void Player::player_alive_info(std::list<Node> *l, struct bufferevent *bev, struct json_object *val, struct event_base *base)
{
	const char *deviceid = json_string(val, "deviceid");

	for (std::list<Node>::iterator it = l->begin(); it != l->end(); ++it)
	{
		if (strcmp(deviceid, it->device_id) == 0)
		{
			if (it->online_flag == 0)
			{
				tNode *t = new tNode;
				struct timeval tv;

				t->l = l;
				std::snprintf(t->id, sizeof(t->id), "%s", deviceid);
				event_assign(&(it->timeout), base, -1, EV_PERSIST, timeout_cb, t);
				evutil_timerclear(&tv);
				tv.tv_sec = 1;
				event_add(&(it->timeout), &tv);
			}

			it->device_bev = bev;
			it->online_flag = 1;
			it->time = time(NULL);
			return;
		}
	}

	std::cout << "alive from unbound device: " << deviceid << std::endl;
}

void Player::player_operation(std::list<Node> *l, struct bufferevent *app_bev, const char *cmd)
{
	const char *device_cmd = device_cmd_from_app_cmd(cmd);

	if (NULL == device_cmd)
	{
		return;
	}

	for (std::list<Node>::iterator it = l->begin(); it != l->end(); ++it)
	{
		if (it->app_bev == app_bev)
		{
			if (it->online_flag == 1 && NULL != it->device_bev)
			{
				struct json_object *request = json_object_new_object();
				json_object_object_add(request, "cmd", json_object_new_string(device_cmd));
				write_json(it->device_bev, request);
			}
			else
			{
				send_offline_reply(app_bev, cmd);
			}

			return;
		}
	}

	send_not_bound_reply(app_bev, cmd);
}

void Player::player_reply_result(std::list<Node> *l, struct bufferevent *bev, struct json_object *val)
{
	const char *cmd = json_string(val, "cmd");
	const char *reply_cmd = NULL;
	struct json_object *reply = NULL;

	if (!strcmp(cmd, "reply"))
	{
		reply_cmd = "app_reply";
	}
	else if (!strcmp(cmd, "reply_music"))
	{
		reply_cmd = "app_reply_music";
	}
	else if (!strcmp(cmd, "reply_status"))
	{
		reply_cmd = "app_reply_status";
	}
	else if (!strcmp(cmd, "reply_camera"))
	{
		reply_cmd = "app_reply_camera";
	}

	if (NULL == reply_cmd)
	{
		return;
	}

	reply = json_tokener_parse(json_object_to_json_string(val));
	if (NULL == reply)
	{
		return;
	}

	json_object_object_del(reply, "cmd");
	json_object_object_add(reply, "cmd", json_object_new_string(reply_cmd));

	for (std::list<Node>::iterator it = l->begin(); it != l->end(); ++it)
	{
		if (it->device_bev == bev)
		{
			if (it->app_online_flag == 1 && NULL != it->app_bev)
			{
				write_json(it->app_bev, reply);
			}
			else
			{
				json_object_put(reply);
			}

			return;
		}
	}

	json_object_put(reply);
}

void Player::timeout_cb(evutil_socket_t fd, short event, void *arg)
{
	tNode *t = static_cast<tNode *>(arg);
	time_t now = time(NULL);

	(void)fd;
	(void)event;

	if (NULL == t || NULL == t->l)
	{
		return;
	}

	for (std::list<Node>::iterator it = (t->l)->begin(); it != (t->l)->end(); ++it)
	{
		if (strcmp(it->device_id, t->id) == 0)
		{
			it->online_flag = ((now - it->time) <= 3) ? 1 : 0;

			if (it->app_online_flag == 1 && it->online_flag == 1 && NULL != it->device_bev)
			{
				struct json_object *request = json_object_new_object();
				json_object_object_add(request, "cmd", json_object_new_string("get"));
				write_json(it->device_bev, request);
			}

			return;
		}
	}
}
