/* PipeWire - pw-cat
 *
 * Copyright © 2020 Konsulko Group

 * Author: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
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

#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>
#include <locale.h>

#include <sndfile.h>

#include <spa/param/audio/layout.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include <pipewire/pipewire.h>
#include <pipewire/i18n.h>
#include <pipewire/extensions/metadata.h>

#include "midifile.h"
#include "dsffile.h"

#define DEFAULT_MEDIA_TYPE	"Audio"
#define DEFAULT_MIDI_MEDIA_TYPE	"Midi"
#define DEFAULT_MEDIA_CATEGORY_PLAYBACK	"Playback"
#define DEFAULT_MEDIA_CATEGORY_RECORD	"Capture"
#define DEFAULT_MEDIA_ROLE	"Music"
#define DEFAULT_TARGET		"auto"
#define DEFAULT_LATENCY_PLAY	"100ms"
#define DEFAULT_LATENCY_REC	"none"
#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2
#define DEFAULT_FORMAT		"s16"
#define DEFAULT_VOLUME		1.0
#define DEFAULT_QUALITY		4

enum mode {
	mode_none,
	mode_playback,
	mode_record
};

enum unit {
	unit_none,
	unit_samples,
	unit_sec,
	unit_msec,
	unit_usec,
	unit_nsec,
};

struct data;

typedef int (*fill_fn)(struct data *d, void *dest, unsigned int n_frames);

struct channelmap {
	int n_channels;
	int channels[SPA_AUDIO_MAX_CHANNELS];
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_source *timer;

	enum mode mode;
	bool verbose;
#define TYPE_PCM	0
#define TYPE_MIDI	1
#define TYPE_DSD	2
	int data_type;
	const char *remote_name;
	const char *media_type;
	const char *media_category;
	const char *media_role;
	const char *channel_map;
	const char *format;
	const char *target;
	const char *latency;
	struct pw_properties *props;

	const char *filename;
	SNDFILE *file;

	unsigned int rate;
	int channels;
	struct channelmap channelmap;
	unsigned int stride;
	enum unit latency_unit;
	unsigned int latency_value;
	int quality;

	enum spa_audio_format spa_format;

	float volume;
	bool volume_is_set;

	fill_fn fill;

	struct spa_io_position *position;
	bool drained;
	uint64_t clock_time;

	struct {
		struct midi_file *file;
		struct midi_file_info info;
	} midi;
	struct {
		struct dsf_file *file;
		struct dsf_file_info info;
		struct dsf_layout layout;
	} dsf;
};

#define STR_FMTS "(ulaw|alaw|u8|s8|s16|s32|f32|f64)"

static const struct format_info {
	const char *name;
	int sf_format;
	uint32_t spa_format;
	uint32_t width;
} format_info[] = {
	{  "ulaw", SF_FORMAT_ULAW, SPA_AUDIO_FORMAT_ULAW, 1 },
	{  "alaw", SF_FORMAT_ULAW, SPA_AUDIO_FORMAT_ALAW, 1 },
	{  "s8", SF_FORMAT_PCM_S8, SPA_AUDIO_FORMAT_S8, 1 },
	{  "u8", SF_FORMAT_PCM_U8, SPA_AUDIO_FORMAT_U8, 1 },
	{  "s16", SF_FORMAT_PCM_16, SPA_AUDIO_FORMAT_S16, 2 },
	{  "s24", SF_FORMAT_PCM_24, SPA_AUDIO_FORMAT_S24, 3 },
	{  "s32", SF_FORMAT_PCM_32, SPA_AUDIO_FORMAT_S32, 4 },
	{  "f32", SF_FORMAT_FLOAT, SPA_AUDIO_FORMAT_F32, 4 },
	{  "f64", SF_FORMAT_DOUBLE, SPA_AUDIO_FORMAT_F32, 8 },
};

static const struct format_info *format_info_by_name(const char *str)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++)
		if (spa_streq(str, format_info[i].name))
			return &format_info[i];
	return NULL;
}

static const struct format_info *format_info_by_sf_format(int format)
{
	uint32_t i;
	int sub_type = (format & SF_FORMAT_SUBMASK);
	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++)
		if (format_info[i].sf_format == sub_type)
			return &format_info[i];
	return NULL;
}

static int sf_playback_fill_x8(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	rn = sf_read_raw(d->file, dest, n_frames * d->stride);
	return (int)rn / d->stride;
}

static int sf_playback_fill_s16(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(short) == sizeof(int16_t));
	rn = sf_readf_short(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_s32(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(int) == sizeof(int32_t));
	rn = sf_readf_int(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_f32(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(float) == 4);
	rn = sf_readf_float(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_f64(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(double) == 8);
	rn = sf_readf_double(d->file, dest, n_frames);
	return (int)rn;
}

static inline fill_fn
playback_fill_fn(uint32_t fmt)
{
	switch (fmt) {
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
		return sf_playback_fill_x8;
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
		/* sndfile check */
		if (sizeof(int16_t) != sizeof(short))
			return NULL;
		return sf_playback_fill_s16;
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
		/* sndfile check */
		if (sizeof(int32_t) != sizeof(int))
			return NULL;
		return sf_playback_fill_s32;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
		/* sndfile check */
		if (sizeof(float) != 4)
			return NULL;
		return sf_playback_fill_f32;
	case SPA_AUDIO_FORMAT_F64_LE:
	case SPA_AUDIO_FORMAT_F64_BE:
		if (sizeof(double) != 8)
			return NULL;
		return sf_playback_fill_f64;
	default:
		break;
	}
	return NULL;
}

static int sf_record_fill_x8(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	rn = sf_write_raw(d->file, src, n_frames * d->stride);
	return (int)rn / d->stride;
}

static int sf_record_fill_s16(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(short) == sizeof(int16_t));
	rn = sf_writef_short(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_s32(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(int) == sizeof(int32_t));
	rn = sf_writef_int(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_f32(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(float) == 4);
	rn = sf_writef_float(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_f64(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(double) == 8);
	rn = sf_writef_double(d->file, src, n_frames);
	return (int)rn;
}

static inline fill_fn
record_fill_fn(uint32_t fmt)
{
	switch (fmt) {
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
		return sf_record_fill_x8;
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
		/* sndfile check */
		if (sizeof(int16_t) != sizeof(short))
			return NULL;
		return sf_record_fill_s16;
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
		/* sndfile check */
		if (sizeof(int32_t) != sizeof(int))
			return NULL;
		return sf_record_fill_s32;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
		/* sndfile check */
		if (sizeof(float) != 4)
			return NULL;
		return sf_record_fill_f32;
	case SPA_AUDIO_FORMAT_F64_LE:
	case SPA_AUDIO_FORMAT_F64_BE:
		/* sndfile check */
		if (sizeof(double) != 8)
			return NULL;
		return sf_record_fill_f64;
	default:
		break;
	}
	return NULL;
}

static int channelmap_from_sf(struct channelmap *map)
{
	static const enum spa_audio_channel table[] = {
		[SF_CHANNEL_MAP_MONO] =                  SPA_AUDIO_CHANNEL_MONO,
		[SF_CHANNEL_MAP_LEFT] =                  SPA_AUDIO_CHANNEL_FL, /* libsndfile distinguishes left and front-left, which we don't */
		[SF_CHANNEL_MAP_RIGHT] =                 SPA_AUDIO_CHANNEL_FR,
		[SF_CHANNEL_MAP_CENTER] =                SPA_AUDIO_CHANNEL_FC,
		[SF_CHANNEL_MAP_FRONT_LEFT] =            SPA_AUDIO_CHANNEL_FL,
		[SF_CHANNEL_MAP_FRONT_RIGHT] =           SPA_AUDIO_CHANNEL_FR,
		[SF_CHANNEL_MAP_FRONT_CENTER] =          SPA_AUDIO_CHANNEL_FC,
		[SF_CHANNEL_MAP_REAR_CENTER] =           SPA_AUDIO_CHANNEL_RC,
		[SF_CHANNEL_MAP_REAR_LEFT] =             SPA_AUDIO_CHANNEL_RL,
		[SF_CHANNEL_MAP_REAR_RIGHT] =            SPA_AUDIO_CHANNEL_RR,
		[SF_CHANNEL_MAP_LFE] =                   SPA_AUDIO_CHANNEL_LFE,
		[SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER] =  SPA_AUDIO_CHANNEL_FLC,
		[SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER] = SPA_AUDIO_CHANNEL_FRC,
		[SF_CHANNEL_MAP_SIDE_LEFT] =             SPA_AUDIO_CHANNEL_SL,
		[SF_CHANNEL_MAP_SIDE_RIGHT] =            SPA_AUDIO_CHANNEL_SR,
		[SF_CHANNEL_MAP_TOP_CENTER] =            SPA_AUDIO_CHANNEL_TC,
		[SF_CHANNEL_MAP_TOP_FRONT_LEFT] =        SPA_AUDIO_CHANNEL_TFL,
		[SF_CHANNEL_MAP_TOP_FRONT_RIGHT] =       SPA_AUDIO_CHANNEL_TFR,
		[SF_CHANNEL_MAP_TOP_FRONT_CENTER] =      SPA_AUDIO_CHANNEL_TFC,
		[SF_CHANNEL_MAP_TOP_REAR_LEFT] =         SPA_AUDIO_CHANNEL_TRL,
		[SF_CHANNEL_MAP_TOP_REAR_RIGHT] =        SPA_AUDIO_CHANNEL_TRR,
		[SF_CHANNEL_MAP_TOP_REAR_CENTER] =       SPA_AUDIO_CHANNEL_TRC
	};
	int i;

	for (i = 0; i < map->n_channels; i++) {
		if (map->channels[i] >= 0 && map->channels[i] < (int) SPA_N_ELEMENTS(table))
			map->channels[i] = table[map->channels[i]];
		else
			map->channels[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
	}
	return 0;
}
struct mapping {
	const char *name;
	unsigned int channels;
	unsigned int values[32];
};

static const struct mapping maps[] =
{
	{ "mono",         SPA_AUDIO_LAYOUT_Mono },
	{ "stereo",       SPA_AUDIO_LAYOUT_Stereo },
	{ "surround-21",  SPA_AUDIO_LAYOUT_2_1 },
	{ "quad",         SPA_AUDIO_LAYOUT_Quad },
	{ "surround-22",  SPA_AUDIO_LAYOUT_2_2 },
	{ "surround-40",  SPA_AUDIO_LAYOUT_4_0 },
	{ "surround-31",  SPA_AUDIO_LAYOUT_3_1 },
	{ "surround-41",  SPA_AUDIO_LAYOUT_4_1 },
	{ "surround-50",  SPA_AUDIO_LAYOUT_5_0 },
	{ "surround-51",  SPA_AUDIO_LAYOUT_5_1 },
	{ "surround-51r", SPA_AUDIO_LAYOUT_5_1R },
	{ "surround-70",  SPA_AUDIO_LAYOUT_7_0 },
	{ "surround-71",  SPA_AUDIO_LAYOUT_7_1 },
};

static unsigned int find_channel(const char *name)
{
	int i;

	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static int parse_channelmap(const char *channel_map, struct channelmap *map)
{
	int i, nch;
	char **ch;

	for (i = 0; i < (int) SPA_N_ELEMENTS(maps); i++) {
		if (spa_streq(maps[i].name, channel_map)) {
			map->n_channels = maps[i].channels;
			spa_memcpy(map->channels, &maps[i].values,
					map->n_channels * sizeof(unsigned int));
			return 0;
		}
	}

	ch = pw_split_strv(channel_map, ",", SPA_AUDIO_MAX_CHANNELS, &nch);
	if (ch == NULL)
		return -1;

	map->n_channels = nch;
	for (i = 0; i < map->n_channels; i++) {
		int c = find_channel(ch[i]);
		map->channels[i] = c;
	}
	pw_free_strv(ch);
	return 0;
}

static int channelmap_default(struct channelmap *map, int n_channels)
{
	switch(n_channels) {
	case 1:
		parse_channelmap("mono", map);
		break;
	case 2:
		parse_channelmap("stereo", map);
		break;
	case 3:
		parse_channelmap("surround-21", map);
		break;
	case 4:
		parse_channelmap("quad", map);
		break;
	case 5:
		parse_channelmap("surround-50", map);
		break;
	case 6:
		parse_channelmap("surround-51", map);
		break;
	case 7:
		parse_channelmap("surround-70", map);
		break;
	case 8:
		parse_channelmap("surround-71", map);
		break;
	default:
		n_channels = 0;
		break;
	}
	map->n_channels = n_channels;
	return 0;
}

static void channelmap_print(struct channelmap *map)
{
	int i;

	for (i = 0; i < map->n_channels; i++) {
		const char *name = spa_debug_type_find_name(spa_type_audio_channel, map->channels[i]);
		if (name == NULL)
			name = ":UNK";
		printf("%s%s", spa_debug_type_short_name(name), i + 1 < map->n_channels ? "," : "");
	}
}

static void on_core_info(void *userdata, const struct pw_core_info *info)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("remote %"PRIu32" is named \"%s\"\n",
				info->id, info->name);
}

static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = userdata;

	fprintf(stderr, "remote error: id=%"PRIu32" seq:%d res:%d (%s): %s\n",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.error = on_core_error,
};

static void
on_state_changed(void *userdata, enum pw_stream_state old,
		 enum pw_stream_state state, const char *error)
{
	struct data *data = userdata;
	int ret;

	if (data->verbose)
		printf("stream state changed %s -> %s\n",
				pw_stream_state_as_string(old),
				pw_stream_state_as_string(state));

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		if (!data->volume_is_set) {
			ret = pw_stream_set_control(data->stream,
					SPA_PROP_volume, 1, &data->volume,
					0);
			if (data->verbose)
				printf("stream set volume to %.3f - %s\n", data->volume,
						ret == 0 ? "success" : "FAILED");

			data->volume_is_set = true;
		}
		if (data->verbose) {
			struct timespec timeout = {0, 1}, interval = {1, 0};
			struct pw_loop *l = pw_main_loop_get_loop(data->loop);
			pw_loop_update_timer(l, data->timer, &timeout, &interval, false);
			printf("stream node %"PRIu32"\n",
				pw_stream_get_node_id(data->stream));
		}
		break;
	case PW_STREAM_STATE_PAUSED:
		if (data->verbose) {
			struct timespec timeout = {0, 0}, interval = {0, 0};
			struct pw_loop *l = pw_main_loop_get_loop(data->loop);
			pw_loop_update_timer(l, data->timer, &timeout, &interval, false);
		}
		break;
	case PW_STREAM_STATE_ERROR:
		printf("stream node %"PRIu32" error: %s\n",
				pw_stream_get_node_id(data->stream),
				error);
		pw_main_loop_quit(data->loop);
		break;
	default:
		break;
	}
}

static void
on_io_changed(void *userdata, uint32_t id, void *data, uint32_t size)
{
	struct data *d = userdata;

	switch (id) {
	case SPA_IO_Position:
		d->position = data;
		break;
	default:
		break;
	}
}

static void
on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct data *data = userdata;
	struct spa_audio_info info = { 0 };
	int err;

	if (data->verbose)
		printf("stream param change: %s\n",
			spa_debug_type_find_name(spa_type_param, id));

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	if ((err = spa_format_parse(param, &info.media_type, &info.media_subtype)) < 0)
		return;

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_dsd)
		return;

	if (spa_format_audio_dsd_parse(param, &info.info.dsd) < 0)
		return;

	data->dsf.layout.interleave = info.info.dsd.interleave,
	data->dsf.layout.channels = info.info.dsd.channels;
	data->dsf.layout.lsb = info.info.dsd.bitorder == SPA_PARAM_BITORDER_lsb;

	data->stride = data->dsf.layout.channels * SPA_ABS(data->dsf.layout.interleave);

	if (data->verbose) {
		printf("DSD: channels:%d bitorder:%s interleave:%d stride:%d\n",
				data->dsf.layout.channels,
				data->dsf.layout.lsb ? "lsb" : "msb",
				data->dsf.layout.interleave,
				data->stride);
	}
}

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	struct spa_data *d;
	int n_frames, n_fill_frames;
	uint8_t *p;
	bool have_data;
	uint32_t offset, size;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL)
		return;

	buf = b->buffer;
	d = &buf->datas[0];

	have_data = false;

	if ((p = d->data) == NULL)
		return;

	if (data->mode == mode_playback) {

		n_frames = d->maxsize / data->stride;
		n_frames = SPA_MIN(n_frames, (int)b->requested);

		n_fill_frames = data->fill(data, p, n_frames);

		if (n_fill_frames > 0 || n_frames == 0) {
			d->chunk->offset = 0;
			d->chunk->stride = data->stride;
			d->chunk->size = n_fill_frames * data->stride;
			have_data = true;
			b->size = n_frames;
		} else if (n_fill_frames < 0) {
			fprintf(stderr, "fill error %d\n", n_fill_frames);
		} else {
			if (data->verbose)
				printf("drain start\n");
		}
	} else {
		offset = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offset);

		p += offset;

		n_frames = size / data->stride;

		n_fill_frames = data->fill(data, p, n_frames);

		have_data = true;
	}

	if (have_data) {
		pw_stream_queue_buffer(data->stream, b);
		return;
	}

	if (data->mode == mode_playback)
		pw_stream_flush(data->stream, true);
}

static void on_drained(void *userdata)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("stream drained\n");

	data->drained = true;
	pw_main_loop_quit(data->loop);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.io_changed = on_io_changed,
	.param_changed = on_param_changed,
	.process = on_process,
	.drained = on_drained
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

static void do_print_delay(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	struct pw_time time;
	pw_stream_get_time_n(data->stream, &time, sizeof(time));
	printf("stream time: now:%"PRIi64" rate:%u/%u ticks:%"PRIu64
			" delay:%"PRIi64" queued:%"PRIu64
			" buffered:%"PRIi64" buffers:%u avail:%u\n",
		time.now,
		time.rate.num, time.rate.denom,
		time.ticks, time.delay, time.queued, time.buffered,
		time.queued_buffers, time.avail_buffers);
}

enum {
	OPT_VERSION = 1000,
	OPT_MEDIA_TYPE,
	OPT_MEDIA_CATEGORY,
	OPT_MEDIA_ROLE,
	OPT_TARGET,
	OPT_LATENCY,
	OPT_RATE,
	OPT_CHANNELS,
	OPT_CHANNELMAP,
	OPT_FORMAT,
	OPT_VOLUME,
};

static const struct option long_options[] = {
	{ "help",		no_argument,	   NULL, 'h' },
	{ "version",		no_argument,	   NULL, OPT_VERSION},
	{ "verbose",		no_argument,	   NULL, 'v' },

	{ "record",		no_argument,	   NULL, 'r' },
	{ "playback",		no_argument,	   NULL, 'p' },
	{ "midi",		no_argument,	   NULL, 'm' },

	{ "remote",		required_argument, NULL, 'R' },

	{ "media-type",		required_argument, NULL, OPT_MEDIA_TYPE },
	{ "media-category",	required_argument, NULL, OPT_MEDIA_CATEGORY },
	{ "media-role",		required_argument, NULL, OPT_MEDIA_ROLE },
	{ "target",		required_argument, NULL, OPT_TARGET },
	{ "latency",		required_argument, NULL, OPT_LATENCY },
	{ "properties",		required_argument, NULL, 'P' },

	{ "rate",		required_argument, NULL, OPT_RATE },
	{ "channels",		required_argument, NULL, OPT_CHANNELS },
	{ "channel-map",	required_argument, NULL, OPT_CHANNELMAP },
	{ "format",		required_argument, NULL, OPT_FORMAT },
	{ "volume",		required_argument, NULL, OPT_VOLUME },
	{ "quality",		required_argument, NULL, 'q' },

	{ NULL, 0, NULL, 0 }
};

static void show_usage(const char *name, bool is_error)
{
	FILE *fp;

	fp = is_error ? stderr : stdout;

        fprintf(fp,
	   _("%s [options] [<file>|-]\n"
             "  -h, --help                            Show this help\n"
             "      --version                         Show version\n"
             "  -v, --verbose                         Enable verbose operations\n"
	     "\n"), name);

	fprintf(fp,
           _("  -R, --remote                          Remote daemon name\n"
             "      --media-type                      Set media type (default %s)\n"
             "      --media-category                  Set media category (default %s)\n"
             "      --media-role                      Set media role (default %s)\n"
             "      --target                          Set node target (default %s)\n"
	     "                                          0 means don't link\n"
             "      --latency                         Set node latency (default %s)\n"
	     "                                          Xunit (unit = s, ms, us, ns)\n"
	     "                                          or direct samples (256)\n"
	     "                                          the rate is the one of the source file\n"
	     "  -P  --properties                      Set node properties\n"
	     "\n"),
	     DEFAULT_MEDIA_TYPE,
	     DEFAULT_MEDIA_CATEGORY_PLAYBACK,
	     DEFAULT_MEDIA_ROLE,
	     DEFAULT_TARGET, DEFAULT_LATENCY_PLAY);

	fprintf(fp,
           _("      --rate                            Sample rate (req. for rec) (default %u)\n"
             "      --channels                        Number of channels (req. for rec) (default %u)\n"
             "      --channel-map                     Channel map\n"
	     "                                            one of: \"stereo\", \"surround-51\",... or\n"
	     "                                            comma separated list of channel names: eg. \"FL,FR\"\n"
             "      --format                          Sample format %s (req. for rec) (default %s)\n"
	     "      --volume                          Stream volume 0-1.0 (default %.3f)\n"
	     "  -q  --quality                         Resampler quality (0 - 15) (default %d)\n"
	     "\n"),
	     DEFAULT_RATE,
	     DEFAULT_CHANNELS,
	     STR_FMTS, DEFAULT_FORMAT,
	     DEFAULT_VOLUME,
	     DEFAULT_QUALITY);

	if (spa_streq(name, "pw-cat")) {
		fputs(
		   _("  -p, --playback                        Playback mode\n"
		     "  -r, --record                          Recording mode\n"
		     "  -m, --midi                            Midi mode\n"
		     "  -d, --dsd                             DSD mode\n"
		     "\n"), fp);
	}
}

static int midi_play(struct data *d, void *src, unsigned int n_frames)
{
	int res;
	struct spa_pod_builder b;
	struct spa_pod_frame f;
	uint32_t first_frame, last_frame;
	bool have_data = false;

	spa_zero(b);
	spa_pod_builder_init(&b, src, n_frames);

        spa_pod_builder_push_sequence(&b, &f, 0);

	first_frame = d->clock_time;
	last_frame = first_frame + d->position->clock.duration;
	d->clock_time = last_frame;

	while (1) {
		uint32_t frame;
		struct midi_event ev;

		res = midi_file_next_time(d->midi.file, &ev.sec);
		if (res <= 0) {
			if (have_data)
				break;
			return res;
		}

		frame = ev.sec * d->position->clock.rate.denom;
		if (frame < first_frame)
			frame = 0;
		else if (frame < last_frame)
			frame -= first_frame;
		else
			break;

		midi_file_read_event(d->midi.file, &ev);

		if (d->verbose)
			midi_file_dump_event(stdout, &ev);

		if (ev.data[0] == 0xff)
			continue;

		spa_pod_builder_control(&b, frame, SPA_CONTROL_Midi);
		spa_pod_builder_bytes(&b, ev.data, ev.size);
		have_data = true;
	}
	spa_pod_builder_pop(&b, &f);

	return b.state.offset;
}

static int midi_record(struct data *d, void *src, unsigned int n_frames)
{
	struct spa_pod *pod;
	struct spa_pod_control *c;
	uint32_t frame;

	frame = d->clock_time;
	d->clock_time += d->position->clock.duration;

	if ((pod = spa_pod_from_data(src, n_frames, 0, n_frames)) == NULL)
		return 0;
	if (!spa_pod_is_sequence(pod))
		return 0;

	SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
		struct midi_event ev;

		if (c->type != SPA_CONTROL_Midi)
			continue;

		ev.track = 0;
		ev.sec = (frame + c->offset) / (float) d->position->clock.rate.denom;
		ev.data = SPA_POD_BODY(&c->value),
		ev.size = SPA_POD_BODY_SIZE(&c->value);

		if (d->verbose)
			midi_file_dump_event(stdout, &ev);

		midi_file_write_event(d->midi.file, &ev);
	}
	return 0;
}

static int setup_midifile(struct data *data)
{
	if (data->mode == mode_record) {
		spa_zero(data->midi.info);
		data->midi.info.format = 0;
		data->midi.info.ntracks = 1;
		data->midi.info.division = 0;
	}

	data->midi.file = midi_file_open(data->filename,
			data->mode == mode_playback ? "r" : "w",
			&data->midi.info);
	if (data->midi.file == NULL) {
		fprintf(stderr, "midifile: can't read midi file '%s': %m\n", data->filename);
		return -errno;
	}

	if (data->verbose)
		printf("midifile: opened file \"%s\" format %08x ntracks:%d div:%d\n",
				data->filename,
				data->midi.info.format, data->midi.info.ntracks,
				data->midi.info.division);

	data->fill = data->mode == mode_playback ?  midi_play : midi_record;
	data->stride = 1;

	return 0;
}

struct dsd_layout_info {
	 uint32_t type;
	 struct spa_audio_layout_info info;
};
static const struct dsd_layout_info dsd_layouts[] = {
	{ 1, { SPA_AUDIO_LAYOUT_Mono, }, },
	{ 2, { SPA_AUDIO_LAYOUT_Stereo, }, },
	{ 3, { SPA_AUDIO_LAYOUT_2FC }, },
	{ 4, { SPA_AUDIO_LAYOUT_Quad }, },
	{ 5, { SPA_AUDIO_LAYOUT_3_1 }, },
	{ 6, { SPA_AUDIO_LAYOUT_5_0R }, },
	{ 7, { SPA_AUDIO_LAYOUT_5_1R }, },
};

static int dsf_play(struct data *d, void *src, unsigned int n_frames)
{
	return dsf_file_read(d->dsf.file, src, n_frames, &d->dsf.layout);
}

static int setup_dsffile(struct data *data)
{
	if (data->mode == mode_record)
		return -ENOTSUP;

	data->dsf.file = dsf_file_open(data->filename, "r", &data->dsf.info);
	if (data->dsf.file == NULL) {
		fprintf(stderr, "dsffile: can't read dsf file '%s': %m\n", data->filename);
		return -errno;
	}

	if (data->verbose)
		printf("dsffile: opened file \"%s\" channels:%d rate:%d samples:%"PRIu64" bitorder:%s\n",
				data->filename,
				data->dsf.info.channels, data->dsf.info.rate,
				data->dsf.info.samples,
				data->dsf.info.lsb ? "lsb" : "msb");

	data->fill = dsf_play;

	return 0;
}

static int stdout_record(struct data *d, void *src, unsigned int n_frames)
{
	return fwrite(src, d->stride, n_frames, stdout);
}

static int stdin_play(struct data *d, void *src, unsigned int n_frames)
{
	return fread(src, d->stride, n_frames, stdin);
}

static int setup_pipe(struct data *data)
{
	const struct format_info *info;

	if (data->format == NULL)
		data->format = DEFAULT_FORMAT;
	if (data->channels == 0)
		data->channels = DEFAULT_CHANNELS;
	if (data->rate == 0)
		data->rate = DEFAULT_RATE;
	if (data->channelmap.n_channels == 0)
		channelmap_default(&data->channelmap, data->channels);

	info = format_info_by_name(data->format);
	if (info == NULL)
		return -EINVAL;

	data->spa_format = info->spa_format;
	data->stride = info->width * data->channels;
	data->fill = data->mode == mode_playback ?  stdin_play : stdout_record;

	if (data->verbose)
		printf("PIPE: rate=%u channels=%u fmt=%s samplesize=%u stride=%u\n",
				data->rate, data->channels,
				info->name, info->width, data->stride);

	return 0;
}

static int fill_properties(struct data *data)
{
	static const char * const table[] = {
		[SF_STR_TITLE] = PW_KEY_MEDIA_TITLE,
		[SF_STR_COPYRIGHT] = PW_KEY_MEDIA_COPYRIGHT,
		[SF_STR_SOFTWARE] = PW_KEY_MEDIA_SOFTWARE,
		[SF_STR_ARTIST] = PW_KEY_MEDIA_ARTIST,
		[SF_STR_COMMENT] = PW_KEY_MEDIA_COMMENT,
		[SF_STR_DATE] = PW_KEY_MEDIA_DATE
	};

	SF_INFO sfi;
	SF_FORMAT_INFO fi;
	int res;
	unsigned c;
	const char *s, *t;

	for (c = 0; c < SPA_N_ELEMENTS(table); c++) {
		if (table[c] == NULL)
			continue;

		if ((s = sf_get_string(data->file, c)) == NULL ||
		    *s == '\0')
			continue;

		pw_properties_set(data->props, table[c], s);
	}

	spa_zero(sfi);
	if ((res = sf_command(data->file, SFC_GET_CURRENT_SF_INFO, &sfi, sizeof(sfi)))) {
		pw_log_error("sndfile: %s", sf_error_number(res));
		return -EIO;
	}

	spa_zero(fi);
	fi.format = sfi.format;
	if (sf_command(data->file, SFC_GET_FORMAT_INFO, &fi, sizeof(fi)) == 0 && fi.name)
		pw_properties_set(data->props, PW_KEY_MEDIA_FORMAT, fi.name);

	s = pw_properties_get(data->props, PW_KEY_MEDIA_TITLE);
	t = pw_properties_get(data->props, PW_KEY_MEDIA_ARTIST);
	if (s && t)
		pw_properties_setf(data->props, PW_KEY_MEDIA_NAME,
				"'%s' / '%s'", s, t);

	return 0;
}
static void format_from_filename(SF_INFO *info, const char *filename)
{
	int i, count = 0;
	int format = -1;

#if __BYTE_ORDER == __BIG_ENDIAN
	info->format |= SF_ENDIAN_BIG;
#else
	info->format |= SF_ENDIAN_LITTLE;
#endif

	if (sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof(int)) != 0)
		count = 0;

	for (i = 0; i < count; i++) {
		SF_FORMAT_INFO fi;

		spa_zero(fi);
		fi.format = i;
		if (sf_command(NULL, SFC_GET_FORMAT_MAJOR, &fi, sizeof(fi)) != 0)
			continue;

		if (spa_strendswith(filename, fi.extension)) {
			format = fi.format;
			break;
		}
	}
	if (format == -1)
		format = SF_FORMAT_WAV;
	if (format == SF_FORMAT_WAV && info->channels > 2)
		format = SF_FORMAT_WAVEX;

	info->format |= format;

	if (format == SF_FORMAT_OGG || format == SF_FORMAT_FLAC)
		info->format = (info->format & ~SF_FORMAT_ENDMASK) | SF_ENDIAN_FILE;
	if (format == SF_FORMAT_OGG)
		info->format = (info->format & ~SF_FORMAT_SUBMASK) | SF_FORMAT_VORBIS;
}

static int setup_sndfile(struct data *data)
{
	const struct format_info *fi = NULL;
	SF_INFO info;

	spa_zero(info);
	/* for record, you fill in the info first */
	if (data->mode == mode_record) {
		if (data->format == NULL)
			data->format = DEFAULT_FORMAT;
		if (data->channels == 0)
			data->channels = DEFAULT_CHANNELS;
		if (data->rate == 0)
			data->rate = DEFAULT_RATE;
		if (data->channelmap.n_channels == 0)
			channelmap_default(&data->channelmap, data->channels);

		if ((fi = format_info_by_name(data->format)) == NULL) {
			fprintf(stderr, "error: unknown format \"%s\"\n", data->format);
			return -EINVAL;
		}
		memset(&info, 0, sizeof(info));
		info.samplerate = data->rate;
		info.channels = data->channels;
		info.format = fi->sf_format;
		format_from_filename(&info, data->filename);
	}

	data->file = sf_open(data->filename,
			data->mode == mode_playback ? SFM_READ : SFM_WRITE,
			&info);
	if (!data->file) {
		fprintf(stderr, "sndfile: failed to open audio file \"%s\": %s\n",
				data->filename, sf_strerror(NULL));
		return -EIO;
	}

	if (data->verbose)
		printf("sndfile: opened file \"%s\" format %08x channels:%d rate:%d\n",
				data->filename, info.format, info.channels, info.samplerate);
	if (data->channels > 0 && info.channels != data->channels) {
		fprintf(stderr, "sndfile: given channels (%u) don't match file channels (%d)\n",
				data->channels, info.channels);
		return -EINVAL;
	}

	data->rate = info.samplerate;
	data->channels = info.channels;

	if (data->mode == mode_playback) {
		if (data->channelmap.n_channels == 0) {
			bool def = false;

			if (sf_command(data->file, SFC_GET_CHANNEL_MAP_INFO,
					data->channelmap.channels,
					sizeof(data->channelmap.channels[0]) * data->channels)) {
				data->channelmap.n_channels = data->channels;
				if (channelmap_from_sf(&data->channelmap) < 0)
					data->channelmap.n_channels = 0;
			}
			if (data->channelmap.n_channels == 0) {
				channelmap_default(&data->channelmap, data->channels);
				def = true;
			}
			if (data->verbose) {
				printf("sndfile: using %s channel map: ", def ? "default" : "file");
				channelmap_print(&data->channelmap);
				printf("\n");
			}
		}
		fill_properties(data);

		/* try native format first, else decode to float */
		if ((fi = format_info_by_sf_format(info.format)) == NULL)
			fi = format_info_by_sf_format(SF_FORMAT_FLOAT);

	}
	if (fi == NULL)
		return -EIO;

	if (data->verbose)
		printf("PCM: fmt:%s rate:%u channels:%u width:%u\n",
				fi->name, data->rate, data->channels, fi->width);

	/* we read and write S24 as S32 with sndfile */
	if (fi->spa_format == SPA_AUDIO_FORMAT_S24)
		fi = format_info_by_sf_format(SF_FORMAT_PCM_32);

	data->spa_format = fi->spa_format;
	data->stride = fi->width * data->channels;
	data->fill = data->mode == mode_playback ?
			playback_fill_fn(data->spa_format) :
			record_fill_fn(data->spa_format);

	if (data->fill == NULL) {
		fprintf(stderr, "PCM: unhandled format %d\n", data->spa_format);
		return -EINVAL;
	}
	return 0;
}

static int setup_properties(struct data *data)
{
	const char *s;
	unsigned int nom = 0;

	if (data->quality >= 0)
		pw_properties_setf(data->props, "resample.quality", "%d", data->quality);

	if (data->rate)
		pw_properties_setf(data->props, PW_KEY_NODE_RATE, "1/%u", data->rate);

	data->latency_unit = unit_none;

	s = data->latency;
	while (*s && isdigit(*s))
		s++;
	if (!*s)
		data->latency_unit = unit_samples;
	else if (spa_streq(s, "none"))
		data->latency_unit = unit_none;
	else if (spa_streq(s, "s") || spa_streq(s, "sec") || spa_streq(s, "secs"))
		data->latency_unit = unit_sec;
	else if (spa_streq(s, "ms") || spa_streq(s, "msec") || spa_streq(s, "msecs"))
		data->latency_unit = unit_msec;
	else if (spa_streq(s, "us") || spa_streq(s, "usec") || spa_streq(s, "usecs"))
		data->latency_unit = unit_usec;
	else if (spa_streq(s, "ns") || spa_streq(s, "nsec") || spa_streq(s, "nsecs"))
		data->latency_unit = unit_nsec;
	else {
		fprintf(stderr, "error: bad latency value %s (bad unit)\n", data->latency);
		return -EINVAL;
	}
	data->latency_value = atoi(data->latency);
	if (!data->latency_value && data->latency_unit != unit_none) {
		fprintf(stderr, "error: bad latency value %s (is zero)\n", data->latency);
		return -EINVAL;
	}

	switch (data->latency_unit) {
	case unit_sec:
		nom = data->latency_value * data->rate;
		break;
	case unit_msec:
		nom = nearbyint((data->latency_value * data->rate) / 1000.0);
		break;
	case unit_usec:
		nom = nearbyint((data->latency_value * data->rate) / 1000000.0);
		break;
	case unit_nsec:
		nom = nearbyint((data->latency_value * data->rate) / 1000000000.0);
		break;
	case unit_samples:
		nom = data->latency_value;
		break;
	default:
		nom = 0;
		break;
	}

	if (data->verbose)
		printf("rate:%d latency:%u (%.3fs)\n",
				data->rate, nom, data->rate ? (double)nom/data->rate : 0.0f);
	if (nom)
		pw_properties_setf(data->props, PW_KEY_NODE_LATENCY, "%u/%u", nom, data->rate);

	return 0;
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	struct pw_loop *l;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const char *prog;
	int exit_code = EXIT_FAILURE, c, ret;
	enum pw_stream_flags flags = 0;

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	flags |= PW_STREAM_FLAG_AUTOCONNECT;

	prog = argv[0];
	if ((prog = strrchr(argv[0], '/')) != NULL)
		prog++;
	else
		prog = argv[0];

	/* prime the mode from the program name */
	if (spa_streq(prog, "pw-play")) {
		data.mode = mode_playback;
		data.data_type = TYPE_PCM;
	} else if (spa_streq(prog, "pw-record")) {
		data.mode = mode_record;
		data.data_type = TYPE_PCM;
	} else if (spa_streq(prog, "pw-midiplay")) {
		data.mode = mode_playback;
		data.data_type = TYPE_MIDI;
	} else if (spa_streq(prog, "pw-midirecord")) {
		data.mode = mode_record;
		data.data_type = TYPE_MIDI;
	} else if (spa_streq(prog, "pw-dsdplay")) {
		data.mode = mode_playback;
		data.data_type = TYPE_DSD;
	} else
		data.mode = mode_none;

	/* negative means no volume adjustment */
	data.volume = -1.0;
	data.quality = -1;
	data.props = pw_properties_new(
			PW_KEY_APP_NAME, prog,
			PW_KEY_NODE_NAME, prog,
			NULL);

	if (data.props == NULL) {
		fprintf(stderr, "error: pw_properties_new() failed: %m\n");
		goto error_no_props;
	}

	while ((c = getopt_long(argc, argv, "hvprmdR:q:P:", long_options, NULL)) != -1) {

		switch (c) {

		case 'h':
			show_usage(prog, false);
			return EXIT_SUCCESS;

		case OPT_VERSION:
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				prog,
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;

		case 'v':
			data.verbose = true;
			break;

		case 'p':
			data.mode = mode_playback;
			break;

		case 'r':
			data.mode = mode_record;
			break;

		case 'm':
			data.data_type = TYPE_MIDI;
			break;

		case 'd':
			data.data_type = TYPE_DSD;
			break;

		case 'R':
			data.remote_name = optarg;
			break;

		case 'q':
			data.quality = atoi(optarg);
			break;

		case OPT_MEDIA_TYPE:
			data.media_type = optarg;
			break;

		case OPT_MEDIA_CATEGORY:
			data.media_category = optarg;
			break;

		case OPT_MEDIA_ROLE:
			data.media_role = optarg;
			break;

		case 'P':
			pw_properties_update_string(data.props, optarg, strlen(optarg));
			break;

		case OPT_TARGET:
			data.target = optarg;
			if (spa_streq(data.target, "0")) {
				data.target = NULL;
				flags &= ~PW_STREAM_FLAG_AUTOCONNECT;
			}
			break;

		case OPT_LATENCY:
			data.latency = optarg;
			break;

		case OPT_RATE:
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad rate %d\n", ret);
				goto error_usage;
			}
			data.rate = (unsigned int)ret;
			break;

		case OPT_CHANNELS:
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad channels %d\n", ret);
				goto error_usage;
			}
			data.channels = (unsigned int)ret;
			break;

		case OPT_CHANNELMAP:
			data.channel_map = optarg;
			break;

		case OPT_FORMAT:
			data.format = optarg;
			break;

		case OPT_VOLUME:
			data.volume = atof(optarg);
			break;
		default:
			goto error_usage;
		}
	}

	if (data.mode == mode_none) {
		fprintf(stderr, "error: one of the playback/record options must be provided\n");
		goto error_usage;
	}

	if (!data.media_type) {
		switch (data.data_type) {
		case TYPE_MIDI:
			data.media_type = DEFAULT_MIDI_MEDIA_TYPE;
			break;
		default:
			data.media_type = DEFAULT_MEDIA_TYPE;
			break;
		}
	}
	if (!data.media_category)
		data.media_category = data.mode == mode_playback ?
					DEFAULT_MEDIA_CATEGORY_PLAYBACK :
					DEFAULT_MEDIA_CATEGORY_RECORD;
	if (!data.media_role)
		data.media_role = DEFAULT_MEDIA_ROLE;

	if (!data.latency)
		data.latency = data.mode == mode_playback ?
			DEFAULT_LATENCY_PLAY :
			DEFAULT_LATENCY_REC;
	if (data.channel_map != NULL) {
		if (parse_channelmap(data.channel_map, &data.channelmap) < 0) {
			fprintf(stderr, "error: can parse channel-map \"%s\"\n", data.channel_map);
			goto error_usage;

		} else {
			if (data.channels > 0 && data.channelmap.n_channels != data.channels) {
				fprintf(stderr, "error: channels and channel-map incompatible\n");
				goto error_usage;
			}
			data.channels = data.channelmap.n_channels;
		}
	}
	if (data.volume < 0)
		data.volume = DEFAULT_VOLUME;

	if (optind >= argc) {
		fprintf(stderr, "error: filename or - argument missing\n");
		goto error_usage;
	}
	data.filename = argv[optind++];

	pw_properties_set(data.props, PW_KEY_MEDIA_TYPE, data.media_type);
	pw_properties_set(data.props, PW_KEY_MEDIA_CATEGORY, data.media_category);
	pw_properties_set(data.props, PW_KEY_MEDIA_ROLE, data.media_role);
	pw_properties_set(data.props, PW_KEY_MEDIA_FILENAME, data.filename);
	pw_properties_set(data.props, PW_KEY_MEDIA_NAME, data.filename);
	pw_properties_set(data.props, PW_KEY_TARGET_OBJECT, data.target);

	/* make a main loop. If you already have another main loop, you can add
	 * the fd of this pipewire mainloop to it. */
	data.loop = pw_main_loop_new(NULL);
	if (!data.loop) {
		fprintf(stderr, "error: pw_main_loop_new() failed: %m\n");
		goto error_no_main_loop;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l,
			pw_properties_new(
				PW_KEY_CONFIG_NAME, "client-rt.conf",
				NULL),
			0);
	if (!data.context) {
		fprintf(stderr, "error: pw_context_new() failed: %m\n");
		goto error_no_context;
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data.remote_name,
				NULL),
			0);
	if (!data.core) {
		fprintf(stderr, "error: pw_context_connect() failed: %m\n");
		goto error_ctx_connect_failed;
	}
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);

	if (spa_streq(data.filename, "-")) {
		ret = setup_pipe(&data);
	} else {
		switch (data.data_type) {
		case TYPE_PCM:
			ret = setup_sndfile(&data);
			break;
		case TYPE_MIDI:
			ret = setup_midifile(&data);
			break;
		case TYPE_DSD:
			ret = setup_dsffile(&data);
			break;
		default:
			ret = -ENOTSUP;
			break;
		}
	}
	if (ret < 0) {
		fprintf(stderr, "error: open failed: %s\n", spa_strerror(ret));
		switch (ret) {
		case -EIO:
			goto error_bad_file;
		case -EINVAL:
		default:
			goto error_usage;
		}
	}
	ret = setup_properties(&data);

	switch (data.data_type) {
	case TYPE_PCM:
	{
		struct spa_audio_info_raw info;
		info = SPA_AUDIO_INFO_RAW_INIT(
			.flags = data.channelmap.n_channels ? 0 : SPA_AUDIO_FLAG_UNPOSITIONED,
			.format = data.spa_format,
			.rate = data.rate,
			.channels = data.channels);

		if (data.channelmap.n_channels)
			memcpy(info.position, data.channelmap.channels, data.channels * sizeof(int));

		params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
		break;
	}
	case TYPE_MIDI:
		params[0] = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,		SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,	SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));

		pw_properties_set(data.props, PW_KEY_FORMAT_DSP, "8 bit raw midi");
		break;
	case TYPE_DSD:
	{
		struct spa_audio_info_dsd info;
		size_t i;

		spa_zero(info);
		info.channels = data.dsf.info.channels;
		info.rate = data.dsf.info.rate / 8;

		for (i = 0; i < SPA_N_ELEMENTS(dsd_layouts); i++) {
			if (dsd_layouts[i].type != data.dsf.info.channel_type)
				continue;
			info.channels = dsd_layouts[i].info.n_channels;
			memcpy(info.position, dsd_layouts[i].info.position,
					info.channels * sizeof(uint32_t));
		}
		params[0] = spa_format_audio_dsd_build(&b, SPA_PARAM_EnumFormat, &info);
		break;
	}
	}

	data.stream = pw_stream_new(data.core, prog, data.props);
	data.props = NULL;

	if (data.stream == NULL) {
		fprintf(stderr, "error: failed to create stream: %m\n");
		goto error_no_stream;
	}
	pw_stream_add_listener(data.stream, &data.stream_listener, &stream_events, &data);

	if (data.verbose)
		printf("connecting %s stream; target=%s\n",
				data.mode == mode_playback ? "playback" : "record",
				data.target);

	if (data.verbose)
		data.timer = pw_loop_add_timer(l, do_print_delay, &data);

	ret = pw_stream_connect(data.stream,
			  data.mode == mode_playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
			  PW_ID_ANY,
			  flags |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 1);
	if (ret < 0) {
		fprintf(stderr, "error: failed connect: %s\n", spa_strerror(ret));
		goto error_connect_fail;
	}

	if (data.verbose) {
		const struct pw_properties *props;
		void *pstate;
		const char *key, *val;

		if ((props = pw_stream_get_properties(data.stream)) != NULL) {
			printf("stream properties:\n");
			pstate = NULL;
			while ((key = pw_properties_iterate(props, &pstate)) != NULL &&
				(val = pw_properties_get(props, key)) != NULL) {
				printf("\t%s = \"%s\"\n", key, val);
			}
		}
	}

	/* and wait while we let things run */
	pw_main_loop_run(data.loop);

	/* we're returning OK only if got to the point to drain */
	if (data.drained)
		exit_code = EXIT_SUCCESS;

error_connect_fail:
	if (data.stream) {
		spa_hook_remove(&data.stream_listener);
		pw_stream_destroy(data.stream);
	}
error_no_stream:
error_bad_file:
	spa_hook_remove(&data.core_listener);
	pw_core_disconnect(data.core);
error_ctx_connect_failed:
	pw_context_destroy(data.context);
error_no_context:
	pw_main_loop_destroy(data.loop);
error_no_props:
error_no_main_loop:
	pw_properties_free(data.props);
	if (data.file)
		sf_close(data.file);
	if (data.midi.file)
		midi_file_close(data.midi.file);
	pw_deinit();
	return exit_code;

error_usage:
	show_usage(prog, true);
	return EXIT_FAILURE;
}
