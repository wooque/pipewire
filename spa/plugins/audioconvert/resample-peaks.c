/* Spa
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

#include <math.h>
#include <errno.h>

#include <spa/param/audio/format.h>

#include "peaks-ops.h"
#include "resample.h"

struct peaks_data {
	uint32_t o_count;
	uint32_t i_count;
	struct peaks peaks;
	float max_f[];
};

static void resample_peaks_process(struct resample *r,
	const void * SPA_RESTRICT src[], uint32_t *in_len,
	void * SPA_RESTRICT dst[], uint32_t *out_len)
{
	struct peaks_data *pd = r->data;
	uint32_t c, i, o, end, chunk, i_count, o_count;

	if (SPA_UNLIKELY(r->channels == 0))
		return;

	for (c = 0; c < r->channels; c++) {
		const float *s = src[c];
		float *d = dst[c], m = pd->max_f[c];

		o_count = pd->o_count;
		i_count = pd->i_count;
		o = i = 0;

		while (i < *in_len && o < *out_len) {
			end = ((uint64_t) (o_count + 1)
				* r->i_rate) / r->o_rate;
			end = end > i_count ? end - i_count : 0;
			chunk = SPA_MIN(end, *in_len);

			m = peaks_abs_max(&pd->peaks, &s[i], chunk - i, m);

			i += chunk;

			if (i == end) {
				d[o++] = m;
				m = 0.0f;
				o_count++;
			}
		}
		pd->max_f[c] = m;
	}
	*out_len = o;
	*in_len = i;
	pd->o_count = o_count;
	pd->i_count = i_count + i;

	while (pd->i_count >= r->i_rate) {
		pd->i_count -= r->i_rate;
		pd->o_count -= r->o_rate;
	}
}

static void impl_peaks_free(struct resample *r)
{
	struct peaks_data *d = r->data;
	if (d != NULL) {
		peaks_free(&d->peaks);
		free(d);
	}
	r->data = NULL;
}

static void impl_peaks_update_rate(struct resample *r, double rate)
{
}

static uint32_t impl_peaks_delay (struct resample *r)
{
	return 0;
}

static uint32_t impl_peaks_in_len(struct resample *r, uint32_t out_len)
{
	return out_len;
}

static void impl_peaks_reset (struct resample *r)
{
	struct peaks_data *d = r->data;
	d->i_count = d->o_count = 0;
}

int resample_peaks_init(struct resample *r)
{
	struct peaks_data *d;
	int res;

	r->free = impl_peaks_free;
	r->update_rate = impl_peaks_update_rate;

	d = calloc(1, sizeof(struct peaks_data) + sizeof(float) * r->channels);
	if (d == NULL)
		return -errno;

	d->peaks.log = r->log;
	d->peaks.cpu_flags = r->cpu_flags;
	if ((res = peaks_init(&d->peaks)) < 0) {
		free(d);
		return res;
	}

	r->data = d;
	r->process = resample_peaks_process;
	r->reset = impl_peaks_reset;
	r->delay = impl_peaks_delay;
	r->in_len = impl_peaks_in_len;

	spa_log_debug(r->log, "peaks %p: in:%d out:%d features:%08x:%08x", r,
			r->i_rate, r->o_rate, r->cpu_flags, d->peaks.cpu_flags);

	r->cpu_flags = d->peaks.cpu_flags;
	d->i_count = d->o_count = 0;
	return 0;
}
