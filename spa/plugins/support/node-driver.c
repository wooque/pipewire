/* Spa
 *
 * Copyright © 2020 Wim Taymans
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

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/keys.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/param.h>

#define NAME "driver"

#define DEFAULT_FREEWHEEL	false
#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"

struct props {
	bool freewheel;
	char clock_name[64];
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct props props;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[1];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct spa_io_position *position;
	struct spa_io_clock *clock;

	struct spa_source timer_source;
	struct itimerspec timerspec;

	bool started;
	bool following;
	uint64_t next_time;
};

static void reset_props(struct props *props)
{
	props->freewheel = DEFAULT_FREEWHEEL;
	spa_scnprintf(props->clock_name, sizeof(props->clock_name),
			"%s", DEFAULT_CLOCK_NAME);
}

static void set_timeout(struct impl *this, uint64_t next_time)
{
	spa_log_trace(this->log, "set timeout %"PRIu64, next_time);
	this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
	this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
	spa_system_timerfd_settime(this->data_system,
			this->timer_source.fd, SPA_FD_TIMER_ABSTIME, &this->timerspec, NULL);
}

static int set_timers(struct impl *this)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now)) < 0)
	    return res;
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (this->following) {
		set_timeout(this, 0);
	} else {
		set_timeout(this, this->next_time);
	}
	return 0;
}

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int do_reassign_follower(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;
	set_timers(this);
	return 0;
}

static int reassign_follower(struct impl *this)
{
	bool following;

	if (this->clock)
		SPA_FLAG_UPDATE(this->clock->flags,
				SPA_IO_CLOCK_FLAG_FREEWHEEL, this->props.freewheel);

	if (!this->started)
		return 0;

	following = is_following(this);
	if (following != this->following) {
		spa_log_debug(this->log, NAME" %p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_invoke(this->data_loop, do_reassign_follower, 0, NULL, 0, true, this);
	}
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		if (size > 0 && size < sizeof(struct spa_io_clock))
			return -EINVAL;
		this->clock = data;
		if (this->clock)
			spa_scnprintf(this->clock->name, sizeof(this->clock->name),
					"%s", this->props.clock_name);
		break;
	case SPA_IO_Position:
		if (size > 0 && size < sizeof(struct spa_io_position))
			return -EINVAL;
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	reassign_follower(this);

	return 0;
}

static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t expirations, nsec, duration;
	uint32_t rate;
	int res;

	spa_log_trace(this->log, "timeout");

	if ((res = spa_system_timerfd_read(this->data_system,
				this->timer_source.fd, &expirations)) < 0) {
		if (res != EAGAIN)
			spa_log_error(this->log, NAME " %p: timerfd error: %s",
					this, spa_strerror(res));
		return;
	}

	nsec = this->next_time;

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.duration;
		rate = this->position->clock.rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}

	this->next_time = nsec + duration * SPA_NSEC_PER_SEC / rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = nsec;
		this->clock->position += duration;
		this->clock->duration = duration;
		this->clock->delay = 0;
		this->clock->rate_diff = 1.0;
		this->clock->next_nsec = this->next_time;
	}

	spa_node_call_ready(&this->callbacks,
			SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int do_start(struct impl *this)
{
	if (this->started)
		return 0;

	this->following = is_following(this);
	set_timers(this);
	this->started = true;
	return 0;
}

static int do_stop(struct impl *this)
{
	if (!this->started)
		return 0;
	this->started = false;
	set_timeout(this, 0);
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);


	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		do_start(this);
		break;
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		do_stop(this);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_NODE_DRIVER, "true" },
};

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct timespec now;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_log_trace(this->log, "process %d", this->props.freewheel);

	if (this->props.freewheel) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		this->next_time = SPA_TIMESPEC_TO_NSEC(&now);
		set_timeout(this, this->next_time);
	}
	return SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int do_remove_timer(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *this = user_data;
	spa_loop_remove_source(this->data_loop, &this->timer_source);
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_loop_invoke(this->data_loop, do_remove_timer, 0, NULL, 0, true, this);
	spa_system_close(this->data_system, this->timer_source.fd);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data_loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data_system is needed");
		return -EINVAL;
	}

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 0;
	this->info.max_output_ports = 0;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 0;

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system, CLOCK_MONOTONIC,
							  SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	this->timerspec.it_value.tv_sec = 0;
	this->timerspec.it_value.tv_nsec = 0;
	this->timerspec.it_interval.tv_sec = 0;
	this->timerspec.it_interval.tv_nsec = 0;

	reset_props(&this->props);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "node.freewheel")) {
			this->props.freewheel = spa_atob(s);
		} else if (spa_streq(k, "clock.name")) {
			spa_scnprintf(this->props.clock_name,
				sizeof(this->props.clock_name), "%s", s);
		}
	}
	spa_loop_add_source(this->data_loop, &this->timer_source);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_support_node_driver_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_SUPPORT_NODE_DRIVER,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
