/* Spa BAP LC3 codec
 *
 * Copyright © 2020 Wim Taymans
 * Copyright © 2022 Pauli Virtanen
 * Copyright © 2022 Collabora
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

#include <bits/stdint-uintn.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>

#include <lc3.h>

#include "media-codecs.h"
#include "bap-codec-caps.h"

struct impl {
	lc3_encoder_t enc[LC3_MAX_CHANNELS];
	lc3_decoder_t dec[LC3_MAX_CHANNELS];

	int mtu;
	int samplerate;
	int channels;
	int frame_dus;
	int framelen;
	int samples;
	unsigned int codesize;
};

struct ltv {
	uint8_t  len;
	uint8_t  type;
	uint8_t  value[0];
} __packed;

static int write_ltv(uint8_t *dest, uint8_t type, void* value, size_t len)
{
	struct ltv *ltv = (struct ltv *)dest;

	ltv->len = len + 1;
	ltv->type = type;
	memcpy(ltv->value, value, len);

	return len + 2;
}

static int write_ltv_uint8(uint8_t *dest, uint8_t type, uint8_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int write_ltv_uint16(uint8_t *dest, uint8_t type, uint16_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int write_ltv_uint32(uint8_t *dest, uint8_t type, uint32_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	uint8_t *data = caps;
	uint16_t framelen[2] = {htobs(LC3_MIN_FRAME_BYTES), htobs(LC3_MAX_FRAME_BYTES)};

	data += write_ltv_uint16(data, LC3_TYPE_FREQ,
	                         htobs(LC3_FREQ_48KHZ | LC3_FREQ_24KHZ | LC3_FREQ_16KHZ | LC3_FREQ_8KHZ));
	data += write_ltv_uint8(data, LC3_TYPE_DUR, LC3_DUR_ANY);
	data += write_ltv_uint8(data, LC3_TYPE_CHAN, LC3_CHAN_1 | LC3_CHAN_2);
	data += write_ltv(data, LC3_TYPE_FRAMELEN, framelen, sizeof(framelen));
	data += write_ltv_uint8(data, LC3_TYPE_BLKS, 2);

	return data - caps;
}

static bool parse_capabilities(bap_lc3_t *conf, const uint8_t *data, size_t data_size)
{
	uint16_t framelen_min = 0, framelen_max = 0;

	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->frame_duration = 0xFF;

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len > data_size)
			return false;

		switch (ltv->type) {
		case LC3_TYPE_FREQ:
			spa_return_val_if_fail(ltv->len == 3, false);
			{
				uint16_t rate = ltv->value[0] + (ltv->value[1] << 8);
				if (rate & LC3_FREQ_48KHZ)
					conf->rate = LC3_CONFIG_FREQ_48KHZ;
				else if (rate & LC3_FREQ_24KHZ)
					conf->rate = LC3_CONFIG_FREQ_24KHZ;
				else if (rate & LC3_FREQ_16KHZ)
					conf->rate = LC3_CONFIG_FREQ_16KHZ;
				else if (rate & LC3_FREQ_8KHZ)
					conf->rate = LC3_CONFIG_FREQ_8KHZ;
				else
					return false;
			}
			break;
		case LC3_TYPE_DUR:
			spa_return_val_if_fail(ltv->len == 2, false);
			{
				uint8_t duration = ltv->value[0];
				if (duration & LC3_DUR_10)
					conf->frame_duration = LC3_CONFIG_DURATION_10;
				else if (duration & LC3_DUR_7_5)
					conf->frame_duration = LC3_CONFIG_DURATION_7_5;
				else
					return false;
			}
			break;
		case LC3_TYPE_CHAN:
			spa_return_val_if_fail(ltv->len == 2, false);
			{
				uint8_t channels = ltv->value[0];
				/* Only mono or stereo streams are currently supported,
				 * in both case Audio location is defined as both Front Left
				 * and Front Right, difference is done by the n_blks parameter.
				 */
				if ((channels & LC3_CHAN_2) || (channels & LC3_CHAN_1))
					conf->channels = LC3_CONFIG_CHNL_FR | LC3_CONFIG_CHNL_FL;
				else
					return false;
			}
			break;
		case LC3_TYPE_FRAMELEN:
			spa_return_val_if_fail(ltv->len == 5, false);
			framelen_min = ltv->value[0] + (ltv->value[1] << 8);
			framelen_max = ltv->value[2] + (ltv->value[3] << 8);
			break;
		case LC3_TYPE_BLKS:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->n_blks = ltv->value[0];
			if (!conf->n_blks)
				return false;
			break;
		default:
			return false;
		}
		data_size -= ltv->len + 1;
		data += ltv->len + 1;
	}

	if (framelen_min < LC3_MIN_FRAME_BYTES || framelen_max > LC3_MAX_FRAME_BYTES)
		return false;
	if (conf->frame_duration == 0xFF || !conf->rate)
		return false;
	if (!conf->channels)
		conf->channels = LC3_CONFIG_CHNL_FL;

	switch (conf->rate) {
	case LC3_CONFIG_FREQ_48KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 117;
		else
			conf->framelen = 120;
		break;
	case LC3_CONFIG_FREQ_24KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 45;
		else
			conf->framelen = 60;
		break;
	case LC3_CONFIG_FREQ_16KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 30;
		else
			conf->framelen = 40;
		break;
	case LC3_CONFIG_FREQ_8KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 26;
		else
			conf->framelen = 30;
		break;
	default:
			return false;
	}

	return true;
}

static bool parse_conf(bap_lc3_t *conf, const uint8_t *data, size_t data_size)
{
	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->frame_duration = 0xFF;

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len > data_size)
			return false;

		switch (ltv->type) {
		case LC3_TYPE_FREQ:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->rate = ltv->value[0];
			break;
		case LC3_TYPE_DUR:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->frame_duration = ltv->value[0];
			break;
		case LC3_TYPE_CHAN:
			spa_return_val_if_fail(ltv->len == 5, false);
			conf->channels = ltv->value[0] + (ltv->value[1] << 8) + (ltv->value[2] << 16) + (ltv->value[3] << 24);
			break;
		case LC3_TYPE_FRAMELEN:
			spa_return_val_if_fail(ltv->len == 3, false);
			conf->framelen = ltv->value[0] + (ltv->value[1] << 8);
			break;
		case LC3_TYPE_BLKS:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->n_blks = ltv->value[0];
			if (!conf->n_blks)
				return false;
			break;
		default:
			return false;
		}
		data_size -= ltv->len + 1;
		data += ltv->len + 1;
	}

	if (conf->frame_duration == 0xFF || !conf->rate)
		return false;

	return true;
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	bap_lc3_t conf;
	uint8_t *data = config;

	if (caps == NULL)
		return -EINVAL;

	if (!parse_capabilities(&conf, caps, caps_size))
		return -ENOTSUP;

	data += write_ltv_uint8(data, LC3_TYPE_FREQ, conf.rate);
	data += write_ltv_uint8(data, LC3_TYPE_DUR, conf.frame_duration);
	data += write_ltv_uint32(data, LC3_TYPE_CHAN, htobl(conf.channels));
	data += write_ltv_uint16(data, LC3_TYPE_FRAMELEN, htobs(conf.framelen));
	data += write_ltv_uint8(data, LC3_TYPE_BLKS, conf.n_blks);

	return data - config;
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info, const struct spa_dict *global_settings)
{
	bap_lc3_t conf1, conf2;
	bap_lc3_t *conf;
	int res1, res2;
	int a, b;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, 0, caps1, caps1_size, info, NULL, (uint8_t *)&conf1);
	res2 = codec->select_config(codec, 0, caps2, caps2_size, info , NULL, (uint8_t *)&conf2);

#define PREFER_EXPR(expr)			\
		do {				\
			conf = &conf1; 		\
			a = (expr);		\
			conf = &conf2;		\
			b = (expr);		\
			if (a != b)		\
				return b - a;	\
		} while (0)

#define PREFER_BOOL(expr)	PREFER_EXPR((expr) ? 1 : 0)

	/* Prefer valid */
	a = (res1 > 0 && (size_t)res1 == sizeof(bap_lc3_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(bap_lc3_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	PREFER_BOOL(conf->channels & LC3_CHAN_2);
	PREFER_BOOL(conf->rate & (LC3_CONFIG_FREQ_48KHZ | LC3_CONFIG_FREQ_24KHZ | LC3_CONFIG_FREQ_16KHZ | LC3_CONFIG_FREQ_8KHZ));
	PREFER_BOOL(conf->rate & LC3_CONFIG_FREQ_48KHZ);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static uint8_t channels_to_positions(uint32_t channels, uint8_t n_channels, uint32_t *position)
{
	uint8_t n_positions = 0;

	spa_assert(n_channels <= SPA_AUDIO_MAX_CHANNELS);

	/* First check if stream is configure for Mono, i.e. 1 block for both Front
	 * Left anf Front Right,
	 * else map LE Audio locations to PipeWire locations in the ascending order
	 * which will be used as block order in stream.
	 */
	if ((channels & (LC3_CONFIG_CHNL_FR | LC3_CONFIG_CHNL_FL)) == (LC3_CONFIG_CHNL_FR | LC3_CONFIG_CHNL_FL) &&
	     n_channels == 1) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		n_positions = 1;
	} else {
#define CHANNEL_2_SPACHANNEL(channel,spa_channel)	if (channels & channel) position[n_positions++] = spa_channel;

		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FL,   SPA_AUDIO_CHANNEL_FL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FR,   SPA_AUDIO_CHANNEL_FR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FC,   SPA_AUDIO_CHANNEL_FC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_LFE,  SPA_AUDIO_CHANNEL_LFE);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BL,   SPA_AUDIO_CHANNEL_RL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BR,   SPA_AUDIO_CHANNEL_RR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FLC,  SPA_AUDIO_CHANNEL_FLC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FRC,  SPA_AUDIO_CHANNEL_FRC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BC,   SPA_AUDIO_CHANNEL_BC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_LFE2, SPA_AUDIO_CHANNEL_LFE2);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_SL,   SPA_AUDIO_CHANNEL_SL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_SR,   SPA_AUDIO_CHANNEL_SR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TFL,  SPA_AUDIO_CHANNEL_TFL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TFR,  SPA_AUDIO_CHANNEL_TFR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TFC,  SPA_AUDIO_CHANNEL_TFC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TC,   SPA_AUDIO_CHANNEL_TC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TBL,  SPA_AUDIO_CHANNEL_TRL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TBR,  SPA_AUDIO_CHANNEL_TRR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TSL,  SPA_AUDIO_CHANNEL_TSL);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TSR,  SPA_AUDIO_CHANNEL_TSR);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_TBC,  SPA_AUDIO_CHANNEL_TRC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BFC,  SPA_AUDIO_CHANNEL_BC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BFL,  SPA_AUDIO_CHANNEL_BLC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_BFR,  SPA_AUDIO_CHANNEL_BRC);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FLW,  SPA_AUDIO_CHANNEL_FLW);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_FRW,  SPA_AUDIO_CHANNEL_FRW);
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_LS,   SPA_AUDIO_CHANNEL_LLFE); /* is it the right mapping? */
		CHANNEL_2_SPACHANNEL(LC3_CONFIG_CHNL_RS,   SPA_AUDIO_CHANNEL_RLFE); /* is it the right mapping? */

#undef CHANNEL_2_SPACHANNEL
	}

	return n_positions;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	bap_lc3_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t i = 0;
	uint8_t res;

	if (!parse_conf(&conf, caps, caps_size))
		return -EINVAL;

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S24_32),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if (conf.rate & LC3_CONFIG_FREQ_48KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.rate & LC3_CONFIG_FREQ_24KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 24000);
		spa_pod_builder_int(b, 24000);
	}
	if (conf.rate & LC3_CONFIG_FREQ_16KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 16000);
		spa_pod_builder_int(b, 16000);
	}
	if (conf.rate & LC3_CONFIG_FREQ_8KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 8000);
		spa_pod_builder_int(b, 8000);
	}
	if (i == 0)
		return -EINVAL;
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	res = channels_to_positions(conf.channels, conf.n_blks, position);
	if (res == 0)
		return -EINVAL;
	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(res),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, res, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	bap_lc3_t conf;
	uint8_t res;

	if (caps == NULL)
		return -EINVAL;

	if (!parse_conf(&conf, caps, caps_size))
		return -ENOTSUP;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S24_32;

	switch (conf.rate) {
	case LC3_CONFIG_FREQ_48KHZ:
		info->info.raw.rate = 48000U;
		break;
	case LC3_CONFIG_FREQ_24KHZ:
		info->info.raw.rate = 24000U;
		break;
	case LC3_CONFIG_FREQ_16KHZ:
		info->info.raw.rate = 16000U;
		break;
	case LC3_CONFIG_FREQ_8KHZ:
		info->info.raw.rate = 8000U;
		break;
	default:
		return -EINVAL;
	}

	res = channels_to_positions(conf.channels, conf.n_blks, info->info.raw.position);
	if (res == 0)
		return -EINVAL;
	info->info.raw.channels = res;

	switch (conf.frame_duration) {
	case LC3_CONFIG_DURATION_10:
	case LC3_CONFIG_DURATION_7_5:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void codec_get_qos(const struct media_codec *codec,
			const void *config, size_t config_size,
			struct codec_qos *qos)
{
	bap_lc3_t conf;

	memset(qos, 0, sizeof(*qos));

	if (!parse_conf(&conf, config, config_size))
		return;

	qos->framing = false;
	qos->phy = "2M";
	qos->retransmission = 2; /* default */
	qos->sdu = conf.framelen * conf.n_blks;
	qos->latency = 20; /* default */
	qos->delay = 40000U;
	qos->interval = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 7500 : 10000);

	switch (conf.rate) {
		case LC3_CONFIG_FREQ_8KHZ:
		case LC3_CONFIG_FREQ_16KHZ:
		case LC3_CONFIG_FREQ_24KHZ:
		case LC3_CONFIG_FREQ_32KHZ:
			qos->retransmission = 2;
			qos->latency = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 8 : 10);
			break;
		case LC3_CONFIG_FREQ_48KHZ:
			qos->retransmission = 5;
			qos->latency = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 15 : 20);
			break;
	}
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	bap_lc3_t conf;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	int res, ich;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S24_32) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	if ((res = codec_validate_config(codec, flags, config, config_len, &config_info)) < 0)
		goto error;

	if (!parse_conf(&conf, config, config_len)) {
		res = -ENOTSUP;
		goto error;
	}

	this->mtu = mtu;
	this->samplerate = config_info.info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->framelen = conf.framelen;

	switch (conf.frame_duration) {
	case LC3_CONFIG_DURATION_10:
		this->frame_dus = 10000;
		break;
	case LC3_CONFIG_DURATION_7_5:
		this->frame_dus = 7500;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	this->samples = lc3_frame_samples(this->frame_dus, this->samplerate);
	if (this->samples < 0) {
		res = -EINVAL;
		goto error;
	}
	this->codesize = this->samples * this->channels * sizeof(int32_t);

	if (flags & MEDIA_CODEC_FLAG_SINK) {
		for (ich = 0; ich < this->channels; ich++) {
			this->enc[ich] = lc3_setup_encoder(this->frame_dus, this->samplerate, 0, calloc(1, lc3_encoder_size(this->frame_dus, this->samplerate)));
			if (this->enc[ich] == NULL) {
				res = -EINVAL;
				goto error;
			}
		}
	} else {
		for (ich = 0; ich < this->channels; ich++) {
			this->dec[ich] = lc3_setup_decoder(this->frame_dus, this->samplerate, 0, calloc(1, lc3_decoder_size(this->frame_dus, this->samplerate)));
			if (this->dec[ich] == NULL) {
				res = -EINVAL;
				goto error;
			}
		}
	}

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this) {
		for (ich = 0; ich < this->channels; ich++) {
			if (this->enc[ich])
				free(this->enc[ich]);
			if (this->dec[ich])
				free(this->dec[ich]);
		}
	}
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	int ich;

	for (ich = 0; ich < this->channels; ich++) {
		if (this->enc[ich])
			free(this->enc[ich]);
		if (this->dec[ich])
			free(this->dec[ich]);
	}
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	return 0;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int frame_bytes;
	int ich, res;
	int size, processed;

	frame_bytes = lc3_frame_bytes(this->frame_dus, this->samplerate);
	processed = 0;
	size = 0;

	if (src_size < (size_t)this->codesize)
		goto done;
	if (dst_size < (size_t)frame_bytes)
		goto done;

	for (ich = 0; ich < this->channels; ich++) {
		uint8_t *in = (uint8_t *)src + (ich * 4);
		uint8_t *out = (uint8_t *)dst + ich * this->framelen;
		res = lc3_encode(this->enc[ich], LC3_PCM_FORMAT_S24, in, this->channels, this->framelen, out);
		size += this->framelen;
		if (SPA_UNLIKELY(res != 0))
			return -EINVAL;
	}
	*dst_out = size;

	processed += this->codesize;

done:
	spa_assert(size <= this->mtu);
	*need_flush = NEED_FLUSH_ALL;

	return processed;
}

static SPA_UNUSED int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	return 0;
}

static SPA_UNUSED int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int ich, res;
	int consumed;
	int samples;

	spa_return_val_if_fail((size_t)(this->framelen * this->channels) == src_size, -EINVAL);
	consumed = 0;

	samples = lc3_frame_samples(this->frame_dus, this->samplerate);
	if (samples == -1)
		return -EINVAL;
	if (dst_size < this->codesize)
		return -EINVAL;

	for (ich = 0; ich < this->channels; ich++) {
		uint8_t *in = (uint8_t *)src + ich * this->framelen;
		uint8_t *out = (uint8_t *)dst + (ich * 4);
		res = lc3_decode(this->dec[ich], in, this->framelen, LC3_PCM_FORMAT_S24, out, this->channels);
		if (SPA_UNLIKELY(res < 0))
			return -EINVAL;
		consumed += this->framelen;
	}

	*dst_out = this->codesize;

	return consumed;
}

static int codec_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

const struct media_codec bap_codec_lc3 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3,
	.name = "lc3",
	.codec_id = BAP_CODEC_LC3,
	.bap = true,
	.description = "LC3",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.get_qos = codec_get_qos,
	.caps_preference_cmp = codec_caps_preference_cmp,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool
};

MEDIA_CODEC_EXPORT_DEF(
	"lc3",
	&bap_codec_lc3
);
