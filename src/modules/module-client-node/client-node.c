/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <spa/support/system.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"
#include "client-node.h"

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/** \cond */

#define MAX_BUFFERS	64
#define MAX_METAS	16u
#define MAX_DATAS	64u
#define MAX_AREAS	2048

#define CHECK_FREE_PORT(this,d,p)	(p <= pw_map_get_size(&this->ports[d]) && !CHECK_PORT(this,d,p))
#define CHECK_PORT(this,d,p)		(pw_map_lookup(&this->ports[d], p) != NULL)
#define GET_PORT(this,d,p)		(pw_map_lookup(&this->ports[d], p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

struct buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[MAX_METAS];
	struct spa_data datas[MAX_DATAS];
	struct pw_memblock *mem;
};

struct mix {
	unsigned int valid:1;
	uint32_t id;
	struct port *port;
	uint32_t peer_id;
	uint32_t n_buffers;
	struct buffer buffers[MAX_BUFFERS];
};

struct params {
	uint32_t n_params;
	struct spa_pod **params;
};

struct port {
	struct pw_impl_port *port;
	struct node *node;
	struct impl *impl;

	enum spa_direction direction;
	uint32_t id;

	struct spa_node mix_node;

	struct spa_port_info info;
	struct pw_properties *properties;

	struct params params;

	unsigned int removed:1;
	unsigned int destroyed:1;

	struct pw_array mix;
};

struct node {
	struct spa_node node;

	struct impl *impl;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct pw_resource *resource;
	struct pw_impl_client *client;

	struct spa_source data_source;
	int writefd;

	struct pw_map ports[2];

	struct port dummy;

	struct params params;
};

struct impl {
	struct pw_impl_client_node this;

	struct pw_context *context;

	struct node node;

	struct pw_map io_map;
	struct pw_memblock *io_areas;

	struct pw_memblock *activation;

	struct spa_hook node_listener;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;

	uint32_t node_id;

	uint32_t bind_node_version;
	uint32_t bind_node_id;

	int fds[2];
	int other_fds[2];
};

#define pw_client_node_resource(r,m,v,...)	\
	pw_resource_call_res(r,struct pw_client_node_events,m,v,__VA_ARGS__)

#define pw_client_node_resource_transport(r,...)	\
	pw_client_node_resource(r,transport,0,__VA_ARGS__)
#define pw_client_node_resource_set_param(r,...)	\
	pw_client_node_resource(r,set_param,0,__VA_ARGS__)
#define pw_client_node_resource_set_io(r,...)	\
	pw_client_node_resource(r,set_io,0,__VA_ARGS__)
#define pw_client_node_resource_event(r,...)	\
	pw_client_node_resource(r,event,0,__VA_ARGS__)
#define pw_client_node_resource_command(r,...)	\
	pw_client_node_resource(r,command,0,__VA_ARGS__)
#define pw_client_node_resource_add_port(r,...)	\
	pw_client_node_resource(r,add_port,0,__VA_ARGS__)
#define pw_client_node_resource_remove_port(r,...)	\
	pw_client_node_resource(r,remove_port,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_param(r,...)	\
	pw_client_node_resource(r,port_set_param,0,__VA_ARGS__)
#define pw_client_node_resource_port_use_buffers(r,...)	\
	pw_client_node_resource(r,port_use_buffers,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_io(r,...)	\
	pw_client_node_resource(r,port_set_io,0,__VA_ARGS__)
#define pw_client_node_resource_set_activation(r,...)	\
	pw_client_node_resource(r,set_activation,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_mix_info(r,...)	\
	pw_client_node_resource(r,port_set_mix_info,1,__VA_ARGS__)

static int update_params(struct params *p, uint32_t n_params, const struct spa_pod **params)
{
	uint32_t i;
	for (i = 0; i < p->n_params; i++)
		free(p->params[i]);
	p->n_params = n_params;
	if (p->n_params == 0) {
		free(p->params);
		p->params = NULL;
	} else {
		struct spa_pod **np;
		np = pw_reallocarray(p->params, p->n_params, sizeof(struct spa_pod *));
		if (np == NULL) {
			pw_log_error("%p: can't realloc: %m", p);
			free(p->params);
			p->params = NULL;
			p->n_params = 0;
			return -errno;
		}
		p->params = np;
	}
	for (i = 0; i < p->n_params; i++)
		p->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
	return 0;
}

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    uint32_t flags,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers);

/** \endcond */

static struct mix *find_mix(struct port *p, uint32_t mix_id)
{
	struct mix *mix;
	size_t len;

	if (mix_id == SPA_ID_INVALID)
		mix_id = 0;
	else
		mix_id++;

	len = pw_array_get_len(&p->mix, struct mix);
	if (mix_id >= len) {
		size_t need = sizeof(struct mix) * (mix_id + 1 - len);
		void *ptr = pw_array_add(&p->mix, need);
		if (ptr == NULL)
			return NULL;
		memset(ptr, 0, need);
	}
	mix = pw_array_get_unchecked(&p->mix, mix_id, struct mix);
	return mix;
}

static void mix_init(struct mix *mix, struct port *p, uint32_t id)
{
	mix->valid = true;
	mix->id = id;
	mix->port = p;
	mix->n_buffers = 0;
}

static struct mix *ensure_mix(struct impl *impl, struct port *p, uint32_t mix_id)
{
	struct mix *mix;

	if ((mix = find_mix(p, mix_id)) == NULL)
		return NULL;
	if (mix->valid)
		return mix;
	mix_init(mix, p, mix_id);
	return mix;
}

static void clear_data(struct node *this, struct spa_data *d)
{
	struct impl *impl = this->impl;

	switch (d->type) {
	case SPA_DATA_MemId:
	{
		uint32_t id;
		struct pw_memblock *m;

		id = SPA_PTR_TO_UINT32(d->data);
		m = pw_mempool_find_id(this->client->pool, id);
		if (m) {
			pw_log_debug("%p: mem %d", impl, m->id);
			pw_memblock_unref(m);
		}
		break;
	}
	case SPA_DATA_MemFd:
	case SPA_DATA_DmaBuf:
		pw_log_debug("%p: close fd:%d", impl, (int)d->fd);
		close(d->fd);
		break;
	default:
		break;
	}
}

static int clear_buffers(struct node *this, struct mix *mix)
{
	uint32_t i, j;

	for (i = 0; i < mix->n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];

		spa_log_debug(this->log, "%p: clear buffer %d", this, i);

		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_data *d = &b->datas[j];
			clear_data(this, d);
		}
		pw_memblock_unref(b->mem);
	}
	mix->n_buffers = 0;
	return 0;
}

static void mix_clear(struct node *this, struct mix *mix)
{
	struct port *port = mix->port;

	if (!mix->valid)
		return;
	do_port_use_buffers(this->impl, port->direction, port->id,
			mix->id, 0, NULL, 0);
	mix->valid = false;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct node *this = object;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	struct spa_result_node_params result;
	uint32_t count = 0;
	bool found = false;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = 0;

	while (true) {
		struct spa_pod *param;

		result.index = result.next++;
		if (result.index >= this->params.n_params)
			break;

		param = this->params.params[result.index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		found = true;

		if (result.index < start)
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result.param, param, filter) == 0) {
			pw_log_debug("%p: %d param %u", this, seq, result.index);
			spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return found ? 0 : -ENOENT;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (this->resource == NULL)
		return param == NULL ? 0 : -EIO;

	return pw_client_node_resource_set_param(this->resource, id, flags, param);
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct node *this = object;
	struct impl *impl = this->impl;
	struct pw_memmap *mm, *old;
	uint32_t memid, mem_offset, mem_size;
	uint32_t tag[5] = { impl->node_id, id, };

	if (impl->this.flags & 1)
		return 0;

	old = pw_mempool_find_tag(this->client->pool, tag, sizeof(tag));

	if (data) {
		mm = pw_mempool_import_map(this->client->pool,
				impl->context->pool, data, size, tag);
		if (mm == NULL)
			return -errno;

		mem_offset = mm->offset;
		memid = mm->block->id;
		mem_size = size;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}
	pw_memmap_free(old);

	if (this->resource == NULL)
		return data == NULL ? 0 : -EIO;

	return pw_client_node_resource_set_io(this->resource,
				       id,
				       memid,
				       mem_offset, mem_size);
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	pw_log_debug("%p: send command %d", this, SPA_COMMAND_TYPE(command));

	if (this->resource == NULL)
		return -EIO;

	return pw_client_node_resource_command(this->resource, command);
}


static void emit_port_info(struct node *this, struct port *port)
{
	spa_node_emit_port_info(&this->hooks,
				port->direction, port->id, &port->info);
}

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct node *this = object;
	struct spa_hook_list save;
	union pw_map_item *item;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	pw_array_for_each(item, &this->ports[SPA_DIRECTION_INPUT].items) {
		if (item->data)
			emit_port_info(this, item->data);
	}
	pw_array_for_each(item, &this->ports[SPA_DIRECTION_OUTPUT].items) {
		if (item->data)
			emit_port_info(this, item->data);
	}
	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_sync(void *object, int seq)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	pw_log_debug("%p: sync", this);

	if (this->resource == NULL)
		return -EIO;

	return pw_resource_ping(this->resource, seq);
}

static void
do_update_port(struct node *this,
	       struct port *port,
	       uint32_t change_mask,
	       uint32_t n_params,
	       const struct spa_pod **params,
	       const struct spa_port_info *info)
{
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		spa_log_debug(this->log, "%p: port %u update %d params", this, port->id, n_params);
		update_params(&port->params, n_params, params);
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		pw_properties_free(port->properties);
		port->properties = NULL;
		port->info.props = NULL;
		port->info.n_params = 0;
		port->info.params = NULL;

		if (info) {
			port->info = *info;
			if (info->props) {
				port->properties = pw_properties_new_dict(info->props);
				port->info.props = &port->properties->dict;
			}
			port->info.n_params = 0;
			port->info.params = NULL;
			spa_node_emit_port_info(&this->hooks, port->direction, port->id, info);
		}
	}
}

static void
clear_port(struct node *this, struct port *port)
{
	struct mix *mix;

	spa_log_debug(this->log, "%p: clear port %p", this, port);

	do_update_port(this, port,
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL);

	pw_array_for_each(mix, &port->mix)
		mix_clear(this, mix);
	pw_array_clear(&port->mix);
	pw_array_init(&port->mix, sizeof(struct mix) * 2);

	pw_map_insert_at(&this->ports[port->direction], port->id, NULL);

	if (!port->removed)
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, NULL);
}

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_FREE_PORT(this, direction, port_id), -EINVAL);

	if (this->resource == NULL)
		return -EIO;

	return pw_client_node_resource_add_port(this->resource, direction, port_id, props);
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->resource == NULL)
		return -EIO;

	return pw_client_node_resource_remove_port(this->resource, direction, port_id);
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct node *this = object;
	struct port *port;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	struct spa_result_node_params result;
	uint32_t count = 0;
	bool found = false;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	port = GET_PORT(this, direction, port_id);
	spa_return_val_if_fail(port != NULL, -EINVAL);

	pw_log_debug("%p: seq:%d port %d.%d id:%u start:%u num:%u n_params:%d",
			this, seq, direction, port_id, id, start, num, port->params.n_params);

	result.id = id;
	result.next = 0;

	while (true) {
		struct spa_pod *param;

		result.index = result.next++;
		if (result.index >= port->params.n_params)
			break;

		param = port->params.params[result.index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		found = true;

		if (result.index < start)
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result.param, param, filter) == 0) {
			pw_log_debug("%p: %d param %u", this, seq, result.index);
			spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return found ? 0 : -ENOENT;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct node *this = object;
	struct port *port;
	struct mix *mix;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	port = GET_PORT(this, direction, port_id);
	if(port == NULL)
		return param == NULL ? 0 : -EINVAL;

	pw_log_debug("%p: port %d.%d set param %s %d", this,
			direction, port_id,
			spa_debug_type_find_name(spa_type_param, id), id);

	if (id == SPA_PARAM_Format) {
		pw_array_for_each(mix, &port->mix)
			clear_buffers(this, mix);
	}
	if (this->resource == NULL)
		return param == NULL ? 0 : -EIO;

	return pw_client_node_resource_port_set_param(this->resource,
					       direction, port_id,
					       id, flags,
					       param);
}

static int do_port_set_io(struct impl *impl,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t mix_id,
			  uint32_t id, void *data, size_t size)
{
	struct node *this = &impl->node;
	uint32_t memid, mem_offset, mem_size;
	struct port *port;
	struct mix *mix;
	uint32_t tag[5] = { impl->node_id, direction, port_id, mix_id, id };
	struct pw_memmap *mm, *old;

	pw_log_debug("%p: %s port %d.%d set io %p %zd", this,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, data, size);

	port = GET_PORT(this, direction, port_id);
	if (port == NULL)
		return data == NULL ? 0 : -EINVAL;

	if ((mix = find_mix(port, mix_id)) == NULL || !mix->valid)
		return -EINVAL;

	old = pw_mempool_find_tag(this->client->pool, tag, sizeof(tag));

	if (data) {
		mm = pw_mempool_import_map(this->client->pool,
				impl->context->pool, data, size, tag);
		if (mm == NULL)
			return -errno;

		mem_offset = mm->offset;
		memid = mm->block->id;
		mem_size = size;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}
	pw_memmap_free(old);

	if (this->resource == NULL)
		return data == NULL ? 0 : -EIO;

	return pw_client_node_resource_port_set_io(this->resource,
					    direction, port_id,
					    mix_id,
					    id,
					    memid,
					    mem_offset, mem_size);
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	/* ignore io on the node itself, we only care about the io on the
	 * port mixers, the io on the node ports itself is handled on the
	 * client side */
	return -EINVAL;
}

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    uint32_t flags,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers)
{
	struct node *this = &impl->node;
	struct port *p;
	struct mix *mix;
	uint32_t i, j;
	struct pw_client_node_buffer *mb;

	p = GET_PORT(this, direction, port_id);
	if (p == NULL)
		return n_buffers == 0 ? 0 : -EINVAL;

	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	spa_log_debug(this->log, "%p: %s port %d.%d use buffers %p %u flags:%08x", this,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, buffers, n_buffers, flags);

	if ((mix = find_mix(p, mix_id)) == NULL || !mix->valid)
		return -EINVAL;

	if (direction == SPA_DIRECTION_OUTPUT) {
		mix_id = SPA_ID_INVALID;
		if ((mix = find_mix(p, mix_id)) == NULL || !mix->valid)
			return -EINVAL;
	}

	clear_buffers(this, mix);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	if (this->resource == NULL)
		return n_buffers == 0 ? 0 : -EIO;

	if (p->destroyed)
		return 0;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];
		struct pw_memblock *mem, *m;
		void *baseptr, *endptr;

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		if (buffers[i]->n_metas > 0)
			baseptr = buffers[i]->metas[0].data;
		else if (buffers[i]->n_datas > 0)
			baseptr = buffers[i]->datas[0].chunk;
		else
			return -EINVAL;

		if ((mem = pw_mempool_find_ptr(impl->context->pool, baseptr)) == NULL)
			return -EINVAL;

		endptr = SPA_PTROFF(baseptr, buffers[i]->n_datas * sizeof(struct spa_chunk), void);
		for (j = 0; j < buffers[i]->n_metas; j++) {
			endptr = SPA_PTROFF(endptr, SPA_ROUND_UP_N(buffers[i]->metas[j].size, 8), void);
		}
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];
			if (d->type == SPA_DATA_MemPtr) {
				if ((m = pw_mempool_find_ptr(impl->context->pool, d->data)) == NULL ||
				    m != mem)
					return -EINVAL;
				endptr = SPA_MAX(endptr, SPA_PTROFF(d->data, d->maxsize, void));
			}
		}
		if (endptr > SPA_PTROFF(baseptr, mem->size, void))
			return -EINVAL;

		m = pw_mempool_import_block(this->client->pool, mem);
		if (m == NULL)
			return -errno;

		b->mem = m;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = m->id;
		mb[i].offset = SPA_PTRDIFF(baseptr, mem->map->ptr);
		mb[i].size = SPA_PTRDIFF(endptr, baseptr);
		spa_log_debug(this->log, "%p: buffer %d %d %d %d", this, i, mb[i].mem_id,
				mb[i].offset, mb[i].size);

		b->buffer.n_metas = SPA_MIN(buffers[i]->n_metas, MAX_METAS);
		for (j = 0; j < b->buffer.n_metas; j++)
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));

		b->buffer.n_datas = SPA_MIN(buffers[i]->n_datas, MAX_DATAS);
		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->datas[j], d, sizeof(struct spa_data));

			if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC)
				continue;

			switch (d->type) {
			case SPA_DATA_DmaBuf:
			case SPA_DATA_MemFd:
			{
				uint32_t flags = PW_MEMBLOCK_FLAG_DONT_CLOSE;

				if (d->flags & SPA_DATA_FLAG_READABLE)
					flags |= PW_MEMBLOCK_FLAG_READABLE;
				if (d->flags & SPA_DATA_FLAG_WRITABLE)
					flags |= PW_MEMBLOCK_FLAG_WRITABLE;

				spa_log_debug(this->log, "mem %d type:%d fd:%d", j, d->type, (int)d->fd);
				m = pw_mempool_import(this->client->pool,
					flags, d->type, d->fd);
				if (m == NULL)
					return -errno;

				b->datas[j].type = SPA_DATA_MemId;
				b->datas[j].data = SPA_UINT32_TO_PTR(m->id);
				break;
			}
			case SPA_DATA_MemPtr:
				spa_log_debug(this->log, "mem %d %zd", j, SPA_PTRDIFF(d->data, baseptr));
				b->datas[j].data = SPA_INT_TO_PTR(SPA_PTRDIFF(d->data, baseptr));
				break;
			default:
				b->datas[j].type = SPA_ID_INVALID;
				b->datas[j].data = NULL;
				spa_log_error(this->log, "invalid memory type %d", d->type);
				break;
			}
		}
	}
	mix->n_buffers = n_buffers;

	return pw_client_node_resource_port_use_buffers(this->resource,
						 direction, port_id, mix_id, flags,
						 n_buffers, mb);
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	return do_port_use_buffers(impl, direction, port_id,
			SPA_ID_INVALID, flags, buffers, n_buffers);
}

static int
impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	spa_log_trace_fp(this->log, "reuse buffer %d", buffer_id);

	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct node *this = object;
	struct impl *impl = this->impl;
	struct pw_impl_node *n = impl->this.node;
	struct timespec ts;

	spa_log_trace_fp(this->log, "%p: send process driver:%p", this, impl->this.node->driver_node);

	if (SPA_UNLIKELY(spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &ts) < 0))
		spa_zero(ts);

	n->rt.activation->status = PW_NODE_ACTIVATION_TRIGGERED;
	n->rt.activation->signal_time = SPA_TIMESPEC_TO_NSEC(&ts);

	if (SPA_UNLIKELY(spa_system_eventfd_write(this->data_system, this->writefd, 1) < 0))
		spa_log_warn(this->log, "%p: error %m", this);

	return SPA_STATUS_OK;
}

static struct pw_node *
client_node_get_node(void *data,
		   uint32_t version,
		   size_t user_data_size)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	uint32_t new_id = user_data_size;

	pw_log_debug("%p: bind %u/%u", this, new_id, version);

	impl->bind_node_version = version;
	impl->bind_node_id = new_id;
	pw_map_insert_at(&this->client->objects, new_id, NULL);

	return NULL;
}

static int
client_node_update(void *data,
		   uint32_t change_mask,
		   uint32_t n_params,
		   const struct spa_pod **params,
		   const struct spa_node_info *info)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		pw_log_debug("%p: update %d params", this, n_params);
		update_params(&this->params, n_params, params);
	}
	if (change_mask & PW_CLIENT_NODE_UPDATE_INFO) {
		spa_node_emit_info(&this->hooks, info);
	}
	pw_log_debug("%p: got node update", this);
	return 0;
}

static int
client_node_port_update(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct port *port;
	bool remove;

	spa_log_debug(this->log, "%p: got port update change:%08x params:%d",
			this, change_mask, n_params);

	remove = (change_mask == 0);

	port = GET_PORT(this, direction, port_id);

	if (remove) {
		if (port == NULL)
			return 0;
		port->destroyed = true;
		clear_port(this, port);
	} else {
		struct port *target;

		if (port == NULL) {
			if (!CHECK_FREE_PORT(this, direction, port_id))
				return -EINVAL;

			target = &this->dummy;
			spa_zero(this->dummy);
			target->direction = direction;
			target->id = port_id;
		} else
			target = port;

		do_update_port(this,
			       target,
			       change_mask,
			       n_params, params,
			       info);
	}
	return 0;
}

static int client_node_set_active(void *data, bool active)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	spa_log_debug(this->log, "%p: active:%d", this, active);
	return pw_impl_node_set_active(impl->this.node, active);
}

static int client_node_event(void *data, const struct spa_event *event)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	spa_node_emit_event(&this->hooks, event);
	return 0;
}

static int client_node_port_buffers(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t mix_id,
			uint32_t n_buffers,
			struct spa_buffer **buffers)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct port *p;
	struct mix *mix;
	uint32_t i, j;

	spa_log_debug(this->log, "%p: %s port %d.%d buffers %p %u", this,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, buffers, n_buffers);

	p = GET_PORT(this, direction, port_id);
	spa_return_val_if_fail(p != NULL, -EINVAL);

	if (direction == SPA_DIRECTION_OUTPUT)
		mix_id = SPA_ID_INVALID;

	if ((mix = find_mix(p, mix_id)) == NULL || !mix->valid)
		return -EINVAL;

	if (mix->n_buffers != n_buffers)
		return -EINVAL;

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *oldbuf, *newbuf;
		struct buffer *b = &mix->buffers[i];

		oldbuf = b->outbuf;
		newbuf = buffers[i];

		spa_log_debug(this->log, "buffer %d n_datas:%d", i, newbuf->n_datas);

		if (oldbuf->n_datas != newbuf->n_datas)
			return -EINVAL;

		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_chunk *oldchunk = oldbuf->datas[j].chunk;
			struct spa_data *d = &newbuf->datas[j];

			/* overwrite everything except the chunk */
			oldbuf->datas[j] = *d;
			oldbuf->datas[j].chunk = oldchunk;

			b->datas[j].type = d->type;
			b->datas[j].fd = d->fd;

			spa_log_debug(this->log, " data %d type:%d fl:%08x fd:%d, offs:%d max:%d",
					j, d->type, d->flags, (int) d->fd, d->mapoffset,
					d->maxsize);
		}
	}
	mix->n_buffers = n_buffers;

	return 0;
}

static const struct pw_client_node_methods client_node_methods = {
	PW_VERSION_CLIENT_NODE_METHODS,
	.get_node = client_node_get_node,
	.update = client_node_update,
	.port_update = client_node_port_update,
	.set_active = client_node_set_active,
	.event = client_node_event,
	.port_buffers = client_node_port_buffers,
};

static void node_on_data_fd_events(struct spa_source *source)
{
	struct node *this = source->data;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "%p: got error", this);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		uint64_t cmd;
		struct pw_impl_node *node = this->impl->this.node;

		if (SPA_UNLIKELY(spa_system_eventfd_read(this->data_system,
					this->data_source.fd, &cmd) < 0))
			pw_log_warn("%p: read failed %m", this);
		else if (SPA_UNLIKELY(cmd > 1))
			pw_log_info("(%s-%u) client missed %"PRIu64" wakeups",
				node->name, node->info.id, cmd - 1);

		spa_log_trace_fp(this->log, "%p: got ready", this);
		spa_node_call_ready(&this->callbacks, SPA_STATUS_HAVE_DATA);
	}
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int
node_init(struct node *this,
	  struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data-loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data-system is needed");
		return -EINVAL;
	}

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->data_source.func = node_on_data_fd_events;
	this->data_source.data = this;
	this->data_source.fd = -1;
	this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->data_source.rmask = 0;

	return 0;
}

static int node_clear(struct node *this)
{
	update_params(&this->params, 0, NULL);
	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct spa_source *source = user_data;
	spa_loop_remove_source(loop, source);
	return 0;
}

static void client_node_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;
	struct node *node = &impl->node;

	pw_log_debug("%p: destroy", node);

	impl->node.resource = this->resource = NULL;
	spa_hook_remove(&impl->resource_listener);
	spa_hook_remove(&impl->object_listener);

	if (node->data_source.fd != -1) {
		spa_loop_invoke(node->data_loop,
				do_remove_source,
				SPA_ID_INVALID,
				NULL,
				0,
				true,
				&node->data_source);
	}
	if (this->node)
		pw_impl_node_destroy(this->node);
}

static void client_node_resource_error(void *data, int seq, int res, const char *message)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct spa_result_node_error result;

	pw_log_error("%p: error seq:%d %d (%s)", this, seq, res, message);
	result.message = message;
	spa_node_emit_result(&this->hooks, seq, res, SPA_RESULT_TYPE_NODE_ERROR, &result);
}

static void client_node_resource_pong(void *data, int seq)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	pw_log_debug("%p: got pong, emit result %d", this, seq);
	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);
}

void pw_impl_client_node_registered(struct pw_impl_client_node *this, struct pw_global *global)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_impl_node *node = this->node;
	struct pw_impl_client *client = impl->node.client;
	uint32_t node_id = global->id;

	pw_log_debug("%p: %d", &impl->node, node_id);

	impl->activation = pw_mempool_import_block(client->pool, node->activation);
	if (impl->activation == NULL) {
		pw_log_debug("%p: can't import block: %m", &impl->node);
		return;
	}
	impl->node_id = node_id;

	if (this->resource == NULL)
		return;

	pw_resource_set_bound_id(this->resource, node_id);

	pw_client_node_resource_transport(this->resource,
					  impl->other_fds[0],
					  impl->other_fds[1],
					  impl->activation->id,
					  0,
					  sizeof(struct pw_node_activation));

	if (impl->bind_node_id) {
		pw_global_bind(global, client, PW_PERM_ALL,
				impl->bind_node_version, impl->bind_node_id);
	}
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;
	struct node *node = &impl->node;
	struct pw_global *global;
	struct spa_system *data_system = impl->node.data_system;
	size_t size;

	impl->fds[0] = spa_system_eventfd_create(data_system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	impl->fds[1] = spa_system_eventfd_create(data_system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	impl->other_fds[0] = impl->fds[1];
	impl->other_fds[1] = impl->fds[0];
	node->data_source.fd = impl->fds[0];
	node->writefd = impl->fds[1];

	spa_loop_add_source(node->data_loop, &node->data_source);
	pw_log_debug("%p: transport read-fd:%d write-fd:%d", node, impl->fds[0], impl->fds[1]);

	size = sizeof(struct spa_io_buffers) * MAX_AREAS;

	impl->io_areas = pw_mempool_alloc(impl->context->pool,
			PW_MEMBLOCK_FLAG_READWRITE |
			PW_MEMBLOCK_FLAG_MAP |
			PW_MEMBLOCK_FLAG_SEAL,
			SPA_DATA_MemFd, size);
	if (impl->io_areas == NULL)
                return;

	pw_log_debug("%p: io areas %p", node, impl->io_areas->map->ptr);

	if ((global = pw_impl_node_get_global(this->node)) != NULL)
		pw_impl_client_node_registered(this, global);
}

static void node_free(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;
	struct node *node = &impl->node;
	struct spa_system *data_system = node->data_system;
	uint32_t tag[5] = { impl->node_id, };
	struct pw_memmap *mm;

	this->node = NULL;

	pw_log_debug("%p: free", node);
	node_clear(node);

	spa_hook_remove(&impl->node_listener);

	while ((mm = pw_mempool_find_tag(node->client->pool, tag, sizeof(uint32_t))) != NULL)
		pw_memmap_free(mm);

	if (this->resource)
		pw_resource_destroy(this->resource);

	if (impl->activation)
		pw_memblock_unref(impl->activation);
	if (impl->io_areas)
		pw_memblock_unref(impl->io_areas);

	pw_map_clear(&impl->node.ports[0]);
	pw_map_clear(&impl->node.ports[1]);
	pw_map_clear(&impl->io_map);

	if (impl->fds[0] != -1)
		spa_system_close(data_system, impl->fds[0]);
	if (impl->fds[1] != -1)
		spa_system_close(data_system, impl->fds[1]);
	free(impl);
}

static int port_init_mix(void *data, struct pw_impl_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct mix *m;

	if ((m = ensure_mix(impl, port, mix->port.port_id)) == NULL)
		return -ENOMEM;

	mix->id = pw_map_insert_new(&impl->io_map, NULL);
	if (mix->id == SPA_ID_INVALID) {
		m->valid = false;
		return -errno;
	}
	if (mix->id > MAX_AREAS) {
		pw_map_remove(&impl->io_map, mix->id);
		m->valid = false;
		return -ENOMEM;
	}

	mix->io = SPA_PTROFF(impl->io_areas->map->ptr,
			mix->id * sizeof(struct spa_io_buffers), void);
	*mix->io = SPA_IO_BUFFERS_INIT;

	m->peer_id = mix->peer_id;

	pw_log_debug("%p: init mix id:%d io:%p base:%p", impl,
			mix->id, mix->io, impl->io_areas->map->ptr);

	return 0;
}

static int port_release_mix(void *data, struct pw_impl_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct node *this = &impl->node;
	struct mix *m;

	pw_log_debug("%p: remove mix id:%d io:%p base:%p",
			this, mix->id, mix->io, impl->io_areas->map->ptr);

	if ((m = find_mix(port, mix->port.port_id)) == NULL || !m->valid)
		return -EINVAL;

	pw_map_remove(&impl->io_map, mix->id);
	m->valid = false;

	return 0;
}

static const struct pw_impl_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.init_mix = port_init_mix,
	.release_mix = port_release_mix,
};

static int
impl_mix_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct port *port = object;

	if (port->direction != direction)
		return -ENOTSUP;

	return impl_node_port_enum_params(&port->node->node, seq, direction, port->id,
			id, start, num, filter);
}

static int
impl_mix_port_set_param(void *object,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int
impl_mix_add_port(void *object, enum spa_direction direction, uint32_t mix_id,
		const struct spa_dict *props)
{
	struct port *port = object;
	pw_log_debug("%p: add port %d:%d.%d", object, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_remove_port(void *object, enum spa_direction direction, uint32_t mix_id)
{
	struct port *port = object;
	pw_log_debug("%p: remove port %d:%d.%d", object, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t mix_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct port *port = object;
	struct impl *impl = port->impl;

	return do_port_use_buffers(impl, direction, port->id, mix_id, flags, buffers, n_buffers);
}

static int impl_mix_port_set_io(void *object,
			   enum spa_direction direction, uint32_t mix_id,
			   uint32_t id, void *data, size_t size)
{
	struct port *p = object;
	struct pw_impl_port *port = p->port;
	struct impl *impl = port->owner_data;
	struct node *this = &impl->node;
	struct pw_impl_port_mix *mix;

	mix = pw_map_lookup(&port->mix_port_map, mix_id);
	if (mix == NULL)
		return -EINVAL;

	if (id == SPA_IO_Buffers) {
		if (data && size >= sizeof(struct spa_io_buffers))
			mix->io = data;
		else
			mix->io = NULL;

		if (mix->io != NULL && this->resource && this->resource->version >= 4)
			pw_client_node_resource_port_set_mix_info(this->resource,
						 direction, port->port_id,
						 mix->port.port_id, mix->peer_id, NULL);
	}

	return do_port_set_io(impl,
			      direction, port->port_id, mix->port.port_id,
			      id, data, size);
}

static int
impl_mix_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct port *p = object;
	return impl_node_port_reuse_buffer(&p->node->node, p->id, buffer_id);
}

static int impl_mix_process(void *object)
{
	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_port_mix = {
	SPA_VERSION_NODE_METHODS,
	.port_enum_params = impl_mix_port_enum_params,
	.port_set_param = impl_mix_port_set_param,
	.add_port = impl_mix_add_port,
	.remove_port = impl_mix_remove_port,
	.port_use_buffers = impl_mix_port_use_buffers,
	.port_set_io = impl_mix_port_set_io,
	.port_reuse_buffer = impl_mix_port_reuse_buffer,
	.process = impl_mix_process,
};

static void node_port_init(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_impl_port_get_user_data(port);
	struct node *this = &impl->node;

	pw_log_debug("%p: port %p init", this, port);

	*p = this->dummy;
	p->port = port;
	p->node = this;
	p->direction = port->direction;
	p->id = port->port_id;
	p->impl = impl;
	pw_array_init(&p->mix, sizeof(struct mix) * 2);
	p->mix_node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_port_mix, p);
	ensure_mix(impl, p, SPA_ID_INVALID);

	pw_map_insert_at(&this->ports[p->direction], p->id, p);
	return;
}

static void node_port_added(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_impl_port_get_user_data(port);

	port->flags |= PW_IMPL_PORT_FLAG_NO_MIXER;

	port->impl = SPA_CALLBACKS_INIT(&port_impl, p);
	port->owner_data = impl;

	pw_impl_port_set_mix(port, &p->mix_node,
			PW_IMPL_PORT_MIX_FLAG_MULTI |
			PW_IMPL_PORT_MIX_FLAG_MIX_ONLY);
}

static void node_port_removed(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct port *p = pw_impl_port_get_user_data(port);

	pw_log_debug("%p: port %p remove", this, port);

	p->removed = true;
	clear_port(this, p);
}

static void node_peer_added(void *data, struct pw_impl_node *peer)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct pw_memblock *m;

	if (peer == impl->this.node)
		return;

	m = pw_mempool_import_block(this->client->pool, peer->activation);
	if (m == NULL) {
		pw_log_debug("%p: can't ensure mem: %m", this);
		return;
	}
	pw_log_debug("%p: peer %p id:%u added mem_id:%u", &impl->this, peer,
			peer->info.id, m->id);

	if (this->resource == NULL)
		return;

	pw_client_node_resource_set_activation(this->resource,
					  peer->info.id,
					  peer->source.fd,
					  m->id,
					  0,
					  sizeof(struct pw_node_activation));
}

static void node_peer_removed(void *data, struct pw_impl_node *peer)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct pw_memblock *m;

	if (peer == impl->this.node)
		return;

	m = pw_mempool_find_fd(this->client->pool, peer->activation->fd);
	if (m == NULL) {
		pw_log_warn("%p: unknown peer %p fd:%d", this, peer,
			peer->source.fd);
		return;
	}
	pw_log_debug("%p: peer %p %u removed", this, peer,
			peer->info.id);

	if (this->resource != NULL) {
		pw_client_node_resource_set_activation(this->resource,
					  peer->info.id,
					  -1,
					  SPA_ID_INVALID,
					  0,
					  0);
	}

	pw_memblock_unref(m);
}

static void node_driver_changed(void *data, struct pw_impl_node *old, struct pw_impl_node *driver)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	pw_log_debug("%p: driver changed %p -> %p", this, old, driver);

	node_peer_removed(data, old);
	node_peer_added(data, driver);
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.free = node_free,
	.initialized = node_initialized,
	.port_init = node_port_init,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
	.peer_added = node_peer_added,
	.peer_removed = node_peer_removed,
	.driver_changed = node_driver_changed,
};

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_node_resource_destroy,
	.error = client_node_resource_error,
	.pong = client_node_resource_pong,
};

/** Create a new client node
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client node
 *
 * Create a new \ref pw_impl_node.
 *
 * \memberof pw_impl_client_node
 */
struct pw_impl_client_node *pw_impl_client_node_new(struct pw_resource *resource,
					  struct pw_properties *properties,
					  bool do_register)
{
	struct impl *impl;
	struct pw_impl_client_node *this;
	struct pw_impl_client *client = pw_resource_get_client(resource);
	struct pw_context *context = pw_impl_client_get_context(client);
	const struct spa_support *support;
	uint32_t n_support;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto error_exit_cleanup;
	}

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_exit_free;
	}

	pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d", client->global->id);

	this = &impl->this;

	impl->context = context;
	impl->fds[0] = impl->fds[1] = -1;
	pw_log_debug("%p: new", &impl->node);

	support = pw_context_get_support(impl->context, &n_support);
	node_init(&impl->node, NULL, support, n_support);
	impl->node.impl = impl;
	impl->node.resource = resource;
	impl->node.client = client;
	this->flags = do_register ? 0 : 1;

	pw_map_init(&impl->node.ports[0], 64, 64);
	pw_map_init(&impl->node.ports[1], 64, 64);
	pw_map_init(&impl->io_map, 64, 64);

	this->resource = resource;
	this->node = pw_spa_node_new(context,
				     PW_SPA_NODE_FLAG_ASYNC |
				     (do_register ? 0 : PW_SPA_NODE_FLAG_NO_REGISTER),
				     (struct spa_node *)&impl->node.node,
				     NULL,
				     properties, 0);

	if (this->node == NULL)
		goto error_no_node;

	this->node->remote = true;
	this->flags = 0;

	pw_resource_add_listener(this->resource,
				&impl->resource_listener,
				&resource_events,
				impl);
	pw_resource_add_object_listener(this->resource,
				&impl->object_listener,
				&client_node_methods,
				impl);

	this->node->port_user_data_size = sizeof(struct port);

	pw_impl_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

error_no_node:
	res = -errno;
	node_clear(&impl->node);
	properties = NULL;
	goto error_exit_free;

error_exit_free:
	free(impl);
error_exit_cleanup:
	if (resource)
		pw_resource_destroy(resource);
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/** Destroy a client node
 * \param node the client node to destroy
 * \memberof pw_impl_client_node
 */
void pw_impl_client_node_destroy(struct pw_impl_client_node *node)
{
	pw_resource_destroy(node->resource);
}
