/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Arun Raghavan <arun@asymptotic.io>
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

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "echo-cancel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_echo_cancel_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *props;
	struct pw_properties *capture_props;
	struct pw_properties *source_props;
	struct pw_properties *sink_props;
	struct pw_properties *playback_props;

	struct spa_audio_info_raw info;
};

static void module_destroy(void *data)
{
	struct module_echo_cancel_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_echo_cancel_load(struct module *module)
{
	struct module_echo_cancel_data *data = module->user_data;
	FILE *f;
	const char *str;
	char *args;
	size_t size;
	uint32_t i;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	/* Can't just serialise this dict because the "null" method gets
	 * interpreted as a JSON null */
	if ((str = pw_properties_get(data->props, "aec.method")))
		fprintf(f, " aec.method = \"%s\"", str);
	if ((str = pw_properties_get(data->props, "aec.args")))
		fprintf(f, " aec.args = \"%s\"", str);
	if (data->info.rate != 0)
		fprintf(f, " audio.rate = %u", data->info.rate);
	if (data->info.channels != 0) {
		fprintf(f, " audio.channels = %u", data->info.channels);
		if (!(data->info.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
			fprintf(f, " audio.position = [ ");
			for (i = 0; i < data->info.channels; i++)
				fprintf(f, "%s%s", i == 0 ? "" : ",",
					channel_id2name(data->info.position[i]));
			fprintf(f, " ]");
		}
	}
	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } source.props = {");
	pw_properties_serialize_dict(f, &data->source_props->dict, 0);
	fprintf(f, " } sink.props = {");
	pw_properties_serialize_dict(f, &data->sink_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data->playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-echo-cancel",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_echo_cancel_unload(struct module *module)
{
	struct module_echo_cancel_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->props);
	pw_properties_free(d->capture_props);
	pw_properties_free(d->source_props);
	pw_properties_free(d->sink_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct spa_dict_item module_echo_cancel_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Acoustic echo canceller" },
	{ PW_KEY_MODULE_USAGE, "source_name=<name for the source> "
				"source_properties=<properties for the source> "
				"source_master=<name of source to filter> "
				"sink_name=<name for the sink> "
				"sink_properties=<properties for the sink> "
				"sink_master=<name of sink to filter> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> "
				"aec_method=<implementation to use> "
				"aec_args=<parameters for the AEC engine> "
#if 0
				/* These are not implemented because they don't
				 * really make sense in the PipeWire context */
				"format=<sample format> "
				"adjust_time=<how often to readjust rates in s> "
				"adjust_threshold=<how much drift to readjust after in ms> "
				"autoloaded=<set if this module is being loaded automatically> "
				"save_aec=<save AEC data in /tmp> "
				"use_volume_sharing=<yes or no> "
				"use_master_format=<yes or no> "
#endif
	},
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_echo_cancel_prepare(struct module * const module)
{
	struct module_echo_cancel_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *aec_props = NULL, *sink_props = NULL, *source_props = NULL;
	struct pw_properties *playback_props = NULL, *capture_props = NULL;
	const char *str;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	aec_props = pw_properties_new(NULL, NULL);
	capture_props = pw_properties_new(NULL, NULL);
	source_props = pw_properties_new(NULL, NULL);
	sink_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!aec_props || !source_props || !sink_props || !capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	} else {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, "echo-cancel-source");
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, "echo-cancel-sink");
	}

	if ((str = pw_properties_get(props, "source_master")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(capture_props, PW_KEY_TARGET_OBJECT,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(capture_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(capture_props, PW_KEY_TARGET_OBJECT, str);
		}
		pw_properties_set(props, "source_master", NULL);
	}

	if ((str = pw_properties_get(props, "sink_master")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_TARGET_OBJECT, str);
		pw_properties_set(props, "sink_master", NULL);
	}

	if (module_args_to_audioinfo(module->impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(source_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(sink_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if ((str = pw_properties_get(props, "aec_method")) != NULL) {
		pw_properties_set(aec_props, "aec.method", str);
		pw_properties_set(props, "aec_method", NULL);
	}

	if ((str = pw_properties_get(props, "aec_args")) != NULL) {
		pw_properties_set(aec_props, "aec.args", str);
		pw_properties_set(props, "aec_args", NULL);
	}

	d->module = module;
	d->props = aec_props;
	d->capture_props = capture_props;
	d->source_props = source_props;
	d->sink_props = sink_props;
	d->playback_props = playback_props;
	d->info = info;

	return 0;
out:
	pw_properties_free(aec_props);
	pw_properties_free(playback_props);
	pw_properties_free(sink_props);
	pw_properties_free(source_props);
	pw_properties_free(capture_props);

	return res;
}

DEFINE_MODULE_INFO(module_echo_cancel) = {
	.name = "module-echo-cancel",
	.prepare = module_echo_cancel_prepare,
	.load = module_echo_cancel_load,
	.unload = module_echo_cancel_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_echo_cancel_info),
	.data_size = sizeof(struct module_echo_cancel_data),
};
