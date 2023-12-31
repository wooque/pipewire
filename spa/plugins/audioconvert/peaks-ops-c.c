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

#include <math.h>

#include "peaks-ops.h"

void peaks_min_max_c(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float *min, float *max)
{
	uint32_t n;
	float t, mi = *min, ma = *max;
	for (n = 0; n < n_samples; n++) {
		t = src[n];
		mi = fminf(mi, t);
		ma = fmaxf(ma, t);
	}
	*min = mi;
	*max = ma;
}

float peaks_abs_max_c(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float max)
{
	uint32_t n;
	for (n = 0; n < n_samples; n++)
		max = fmaxf(fabsf(src[n]), max);
	return max;
}
