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

#include <math.h>

#include <spa/utils/defs.h>

#include "resample.h"

struct peaks_data {
	uint32_t o_count;
	uint32_t i_count;
	float max_f[];
};

#define DEFINE_PEAKS(arch)						\
void resample_peaks_process_##arch(struct resample *r,			\
	const void * SPA_RESTRICT src[], uint32_t *in_len,		\
	void * SPA_RESTRICT dst[], uint32_t *out_len)

#define MAKE_PEAKS(arch)						\
DEFINE_PEAKS(arch)							\
{									\
	struct peaks_data *pd = r->data;				\
	uint32_t c, i, o, end, chunk, i_count, o_count;			\
									\
	if (SPA_UNLIKELY(r->channels == 0))				\
		return;							\
									\
	for (c = 0; c < r->channels; c++) {				\
		const float *s = src[c];				\
		float *d = dst[c], m = pd->max_f[c];			\
									\
		o_count = pd->o_count;					\
		i_count = pd->i_count;					\
		o = i = 0;						\
									\
		while (i < *in_len && o < *out_len) {			\
			end = ((uint64_t) (o_count + 1) 		\
				* r->i_rate) / r->o_rate;		\
			end = end > i_count ? end - i_count : 0;	\
			chunk = SPA_MIN(end, *in_len);			\
									\
			m = find_abs_max_##arch(&s[i], chunk - i, m);	\
									\
			i += chunk;					\
									\
			if (i == end) {					\
				d[o++] = m;				\
				m = 0.0f;				\
				o_count++;				\
			}						\
		}							\
		pd->max_f[c] = m;					\
	}								\
	*out_len = o;							\
	*in_len = i;							\
	pd->o_count = o_count;						\
	pd->i_count = i_count + i;					\
									\
	while (pd->i_count >= r->i_rate) {				\
		pd->i_count -= r->i_rate;				\
		pd->o_count -= r->o_rate;				\
	}								\
}


DEFINE_PEAKS(c);
#if defined (HAVE_SSE)
DEFINE_PEAKS(sse);
#endif
