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

#include <spa/utils/defs.h>

struct dsp_ops {
	uint32_t cpu_flags;

	void (*clear) (struct dsp_ops *ops, void * SPA_RESTRICT dst, uint32_t n_samples);
	void (*copy) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples);
	void (*mix_gain) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[],
			float gain[], uint32_t n_src, uint32_t n_samples);
	void (*free) (struct dsp_ops *ops);

	const void *priv;
};

int dsp_ops_init(struct dsp_ops *ops);

#define dsp_ops_copy(ops,...)		(ops)->copy(ops, __VA_ARGS__)
#define dsp_ops_mix_gain(ops,...)	(ops)->mix_gain(ops, __VA_ARGS__)
#define dsp_ops_free(ops)		(ops)->free(ops)


#define MAKE_COPY_FUNC(arch) \
void dsp_copy_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst, \
			const void * SPA_RESTRICT src, uint32_t n_samples)
#define MAKE_MIX_GAIN_FUNC(arch) \
void dsp_mix_gain_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst,	\
	const void * SPA_RESTRICT src[], float gain[], uint32_t n_src, uint32_t n_samples)


MAKE_COPY_FUNC(c);
MAKE_MIX_GAIN_FUNC(c);
#if defined (HAVE_SSE)
MAKE_MIX_GAIN_FUNC(sse);
#endif
