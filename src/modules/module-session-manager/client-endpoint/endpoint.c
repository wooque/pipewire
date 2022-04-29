/* PipeWire
 *
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <string.h>

#include <pipewire/impl.h>
#include <pipewire/extensions/session-manager.h>

#include <spa/pod/filter.h>

#include "endpoint.h"
#include "client-endpoint.h"

#define NAME "endpoint"

struct resource_data {
	struct endpoint *endpoint;
	struct spa_hook object_listener;
	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

#define pw_endpoint_resource(r,m,v,...)	\
	pw_resource_call(r,struct pw_endpoint_events,m,v,__VA_ARGS__)
#define pw_endpoint_resource_info(r,...)	\
	pw_endpoint_resource(r,info,0,__VA_ARGS__)
#define pw_endpoint_resource_param(r,...)	\
	pw_endpoint_resource(r,param,0,__VA_ARGS__)

static int endpoint_enum_params (void *object, int seq,
				uint32_t id, uint32_t start, uint32_t num,
				const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint *this = data->endpoint;
	struct spa_pod *result;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	uint32_t index;
	uint32_t next = start;
	uint32_t count = 0;

	pw_log_debug(NAME" %p: param %u %d/%d", this, id, start, num);

	while (true) {
		index = next++;
		if (index >= this->n_params)
			break;

		param = this->params[index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (spa_pod_filter(&b, &result, param, filter) != 0)
			continue;

		pw_log_debug(NAME" %p: %d param %u", this, seq, index);

		pw_endpoint_resource_param(resource, seq, id, index, next, result);

		if (++count == num)
			break;
	}
	return 0;
}

static int endpoint_subscribe_params (void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug(NAME" %p: resource %d subscribe param %u",
			data->endpoint, pw_resource_get_id(resource), ids[i]);
		endpoint_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int endpoint_set_param (void *object, uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint *this = data->endpoint;

	pw_log_debug("%p", this);
	pw_client_endpoint_resource_set_param(this->client_ep->resource,
						id, flags, param);

	return 0;
}

static int endpoint_create_link(void *object, const struct spa_dict *props)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint *this = data->endpoint;

	pw_log_debug("%p", this);
	pw_client_endpoint_resource_create_link(this->client_ep->resource,
						props);

	return 0;
}

static const struct pw_endpoint_methods methods = {
	PW_VERSION_ENDPOINT_METHODS,
	.subscribe_params = endpoint_subscribe_params,
	.enum_params = endpoint_enum_params,
	.set_param = endpoint_set_param,
	.create_link = endpoint_create_link,
};

struct emit_param_data {
	struct endpoint *this;
	struct spa_pod *param;
	uint32_t id;
	uint32_t index;
	uint32_t next;
};

static int emit_param(void *_data, struct pw_resource *resource)
{
	struct emit_param_data *d = _data;
	struct resource_data *data;
	uint32_t i;

	data = pw_resource_get_user_data(resource);
	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == d->id) {
			pw_endpoint_resource_param(resource, 1,
				d->id, d->index, d->next, d->param);
		}
	}
	return 0;
}

static void endpoint_notify_subscribed(struct endpoint *this,
					uint32_t index, uint32_t next)
{
	struct pw_global *global = this->global;
	struct emit_param_data data;
	struct spa_pod *param = this->params[index];

	if (!param || !spa_pod_is_object (param))
		return;

	data.this = this;
	data.param = param;
	data.id = SPA_POD_OBJECT_ID (param);
	data.index = index;
	data.next = next;

	pw_global_for_each_resource(global, emit_param, &data);
}

static int emit_info(void *data, struct pw_resource *resource)
{
	struct endpoint *this = data;
	pw_endpoint_resource_info(resource, &this->info);
	return 0;
}

int endpoint_update(struct endpoint *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_info *info)
{
	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_PARAMS) {
		uint32_t i;

		pw_log_debug(NAME" %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		if (this->n_params == 0) {
			free(this->params);
			this->params = NULL;
		} else {
			void *p;
			p = reallocarray(this->params, n_params, sizeof(struct spa_pod*));
			if (p == NULL) {
				free(this->params);
				this->params = NULL;
				this->n_params = 0;
				goto no_mem;
			}
			this->params = p;
		}
		for (i = 0; i < this->n_params; i++) {
			this->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
			endpoint_notify_subscribed(this, i, i+1);
		}
	}

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO) {
		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS)
			this->info.n_streams = info->n_streams;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION)
			this->info.session_id = info->session_id;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS)
			pw_properties_update(this->props, info->props);

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
			this->info.n_params = info->n_params;
			if (info->n_params == 0) {
				free(this->info.params);
				this->info.params = NULL;
			} else {
				void *p;
				p = reallocarray(this->info.params, info->n_params, sizeof(struct spa_param_info));
				if (p == NULL) {
					free(this->info.params);
					this->info.params = NULL;
					this->info.n_params = 0;
					goto no_mem;
				}
				this->info.params = p;
				memcpy(this->info.params, info->params, info->n_params * sizeof(struct spa_param_info));
			}
		}

		if (!this->info.name) {
			this->info.name = info->name ? strdup(info->name) : NULL;
			this->info.media_class = info->media_class ? strdup(info->media_class) : NULL;
			this->info.direction = info->direction;
			this->info.flags = info->flags;
		}

		this->info.change_mask = info->change_mask;
		pw_global_for_each_resource(this->global, emit_info, this);
		this->info.change_mask = 0;
	}

	return 0;

      no_mem:
	pw_log_error(NAME" can't update: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't update: no memory");
	return -ENOMEM;
}

static int endpoint_bind(void *_data, struct pw_impl_client *client,
			uint32_t permissions, uint32_t version, uint32_t id)
{
	struct endpoint *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
			pw_global_get_type(global), version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->endpoint = this;
	pw_resource_add_object_listener(resource, &data->object_listener,
					&methods, resource);

	pw_log_debug(NAME" %p: bound to %d", this, pw_resource_get_id(resource));
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_ENDPOINT_CHANGE_MASK_ALL;
	pw_endpoint_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error(NAME" can't create resource: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't create resource: no memory");
	return -ENOMEM;
}

int endpoint_init(struct endpoint *this,
		struct client_endpoint *client_ep,
		struct pw_context *context,
		struct pw_properties *properties)
{
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		PW_KEY_DEVICE_ID,
		PW_KEY_NODE_ID,
		PW_KEY_MEDIA_CLASS,
		PW_KEY_SESSION_ID,
		PW_KEY_PRIORITY_SESSION,
		PW_KEY_ENDPOINT_NAME,
		PW_KEY_ENDPOINT_CLIENT_ID,
		PW_KEY_ENDPOINT_ICON_NAME,
		PW_KEY_ENDPOINT_MONITOR,
		NULL
	};

	pw_log_debug(NAME" %p: new", this);

	this->client_ep = client_ep;
	this->props = properties;

	this->global = pw_global_new (context,
			PW_TYPE_INTERFACE_Endpoint,
			PW_VERSION_ENDPOINT,
			NULL, endpoint_bind, this);
	if (!this->global)
		goto no_mem;

	pw_properties_setf(this->props, PW_KEY_OBJECT_ID, "%u",
			pw_global_get_id(this->global));
	pw_properties_setf(this->props, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(this->global));

	this->info.version = PW_VERSION_ENDPOINT_INFO;
	this->info.id = pw_global_get_id(this->global);
	this->info.props = &this->props->dict;

	pw_global_update_keys(this->global, &this->props->dict, keys);

	pw_resource_set_bound_id(client_ep->resource, this->info.id);

	return pw_global_register(this->global);

      no_mem:
	pw_log_error(NAME" - can't create - out of memory");
	return -ENOMEM;
}

void endpoint_clear(struct endpoint *this)
{
	uint32_t i;

	pw_log_debug(NAME" %p: destroy", this);

	pw_global_destroy(this->global);

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	free(this->info.name);
	free(this->info.media_class);
	free(this->info.params);

	pw_properties_free(this->props);
}
