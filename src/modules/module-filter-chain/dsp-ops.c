/* Spa
 *
 * Copyright © 2022 Wim Taymans
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
#include <stdio.h>
#include <math.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "dsp-ops.h"

struct dsp_info {
	uint32_t cpu_flags;

	void (*copy) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples);
	void (*mix_gain) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[],
			float gain[], uint32_t n_src, uint32_t n_samples);
	void (*biquad_run) (struct dsp_ops *ops, struct biquad *bq,
			float *out, const float *in, uint32_t n_samples);
};

static struct dsp_info dsp_table[] =
{
#if defined (HAVE_SSE)
	{ SPA_CPU_FLAG_SSE,
		.copy = dsp_copy_c,
		.mix_gain = dsp_mix_gain_sse,
		.biquad_run = dsp_biquad_run_c,
	},
#endif
	{ 0,
		.copy = dsp_copy_c,
		.mix_gain = dsp_mix_gain_c,
		.biquad_run = dsp_biquad_run_c,
	},
};

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct dsp_info *find_dsp_info(uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(dsp_table, t) {
		if (MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_dsp_ops_free(struct dsp_ops *ops)
{
	spa_zero(*ops);
}

int dsp_ops_init(struct dsp_ops *ops)
{
	const struct dsp_info *info;

	info = find_dsp_info(ops->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	ops->priv = info;
	ops->cpu_flags = info->cpu_flags;
	ops->copy = info->copy;
	ops->mix_gain = info->mix_gain;
	ops->biquad_run = info->biquad_run;
	ops->free = impl_dsp_ops_free;

	return 0;
}
