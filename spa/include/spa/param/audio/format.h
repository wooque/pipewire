/* Simple Plugin API
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

#ifndef SPA_PARAM_AUDIO_FORMAT_H
#define SPA_PARAM_AUDIO_FORMAT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#include <spa/param/format.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/dsp.h>
#include <spa/param/audio/iec958.h>
#include <spa/param/audio/dsd.h>
#include <spa/param/audio/mp3.h>
#include <spa/param/audio/aac.h>
#include <spa/param/audio/vorbis.h>
#include <spa/param/audio/wma.h>
#include <spa/param/audio/ra.h>
#include <spa/param/audio/amr.h>
#include <spa/param/audio/alac.h>
#include <spa/param/audio/flac.h>
#include <spa/param/audio/ape.h>

struct spa_audio_info {
	uint32_t media_type;
	uint32_t media_subtype;
	union {
		struct spa_audio_info_raw raw;
		struct spa_audio_info_dsp dsp;
		struct spa_audio_info_iec958 iec958;
		struct spa_audio_info_dsd dsd;
		struct spa_audio_info_mp3 mp3;
		struct spa_audio_info_aac aac;
		struct spa_audio_info_vorbis vorbis;
		struct spa_audio_info_wma wma;
		struct spa_audio_info_ra ra;
		struct spa_audio_info_amr amr;
		struct spa_audio_info_alac alac;
		struct spa_audio_info_flac flac;
		struct spa_audio_info_ape ape;
	} info;
};

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_AUDIO_FORMAT_H */
