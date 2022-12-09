/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include "../defs.h"
#include "../module.h"

#define NAME "pipe-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_pipesink_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *capture_props;
	struct spa_audio_info_raw info;
	char *filename;
};

static void module_destroy(void *data)
{
	struct module_pipesink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_pipe_sink_load(struct module *module)
{
	struct module_pipesink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;
	uint32_t i;

	pw_properties_setf(data->capture_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	fprintf(f, " \"tunnel.mode\" = \"sink\" ");
	if (data->filename != NULL)
		fprintf(f, " \"pipe.filename\": \"%s\"", data->filename);
	if (data->info.format != 0)
		fprintf(f, " \"audio.format\": \"%s\"", format_id2name(data->info.format));
	if (data->info.rate != 0)
		fprintf(f, " \"audio.rate\": %u,", data->info.rate);
	if (data->info.channels != 0) {
		fprintf(f, " \"audio.channels\": %u,", data->info.channels);
		if (!(data->info.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
			fprintf(f, " \"audio.position\": [ ");
			for (i = 0; i < data->info.channels; i++)
				fprintf(f, "%s\"%s\"", i == 0 ? "" : ",",
					channel_id2name(data->info.position[i]));
			fprintf(f, " ],");
		}
	}
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-pipe-tunnel",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);
	return 0;
}

static int module_pipe_sink_unload(struct module *module)
{
	struct module_pipesink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}
	pw_properties_free(d->capture_props);
	free(d->filename);
	return 0;
}

static const struct spa_dict_item module_pipe_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pipe sink" },
	{ PW_KEY_MODULE_USAGE, "file=<name of the FIFO special file to use> "
				"sink_name=<name for the sink> "
				"sink_properties=<sink properties> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_pipe_sink_prepare(struct module * const module)
{
	struct module_pipesink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *capture_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	char *filename = NULL;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	capture_props = pw_properties_new(NULL, NULL);
	if (!capture_props) {
		res = -EINVAL;
		goto out;
	}

	if (module_args_to_audioinfo(module->impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	info.format = SPA_AUDIO_FORMAT_S16;
	if ((str = pw_properties_get(props, "format")) != NULL) {
		info.format = format_paname2id(str, strlen(str));
		pw_properties_set(props, "format", NULL);
	}
	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	if ((str = pw_properties_get(props, "sink_properties")) != NULL)
		module_args_add_props(capture_props, str);

	if ((str = pw_properties_get(props, "file")) != NULL) {
		filename = strdup(str);
		pw_properties_set(props, "file", NULL);
	}

	d->module = module;
	d->capture_props = capture_props;
	d->info = info;
	d->filename = filename;

	return 0;
out:
	pw_properties_free(capture_props);
	free(filename);
	return res;
}

DEFINE_MODULE_INFO(module_pipe_sink) = {
	.name = "module-pipe-sink",
	.prepare = module_pipe_sink_prepare,
	.load = module_pipe_sink_load,
	.unload = module_pipe_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_pipe_sink_info),
	.data_size = sizeof(struct module_pipesink_data),
};
