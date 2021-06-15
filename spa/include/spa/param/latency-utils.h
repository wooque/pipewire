/* Simple Plugin API
 *
 * Copyright © 2021 Wim Taymans
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

#ifndef SPA_PARAM_LATENCY_UTILS_H
#define SPA_PARAM_LATENCY_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif


#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>

struct spa_latency_info {
	enum spa_direction direction;
	float min_quantum;
	float max_quantum;
	uint32_t min_rate;
	uint32_t max_rate;
	uint64_t min_ns;
	uint64_t max_ns;
};

#define SPA_LATENCY_INFO(dir) (struct spa_latency_info) { .direction = (dir) }

static inline int
spa_latency_info_compare(const struct spa_latency_info *a, struct spa_latency_info *b)
{
	if (a->min_quantum == b->min_quantum &&
	    a->max_quantum == b->max_quantum &&
	    a->min_rate == b->min_rate &&
	    a->max_rate == b->max_rate &&
	    a->min_ns == b->min_ns &&
	    a->max_ns == b->max_ns)
		return 0;
	return 1;
}

static inline int
spa_latency_info_combine(struct spa_latency_info *info, const struct spa_latency_info *other)
{
	if (info->direction != other->direction)
		return -EINVAL;
	if (info->min_quantum == 0.0f || other->min_quantum < info->min_quantum)
		info->min_quantum = other->min_quantum;
	if (other->max_quantum > info->max_quantum)
		info->max_quantum = other->max_quantum;
	if (info->min_rate == 0U || other->min_rate < info->min_rate)
		info->min_rate = other->min_rate;
	if (other->max_rate > info->max_rate)
		info->max_rate = other->max_rate;
	if (info->min_ns == 0UL || other->min_ns < info->min_ns)
		info->min_ns = other->min_ns;
	if (other->max_ns > info->max_ns)
		info->max_ns = other->max_ns;
	return 0;
}

static inline int
spa_latency_parse(const struct spa_pod *latency, struct spa_latency_info *info)
{
	int res;
	spa_zero(*info);
	if ((res = spa_pod_parse_object(latency,
			SPA_TYPE_OBJECT_ParamLatency, NULL,
			SPA_PARAM_LATENCY_direction, SPA_POD_Id(&info->direction),
			SPA_PARAM_LATENCY_minQuantum, SPA_POD_OPT_Float(&info->min_quantum),
			SPA_PARAM_LATENCY_maxQuantum, SPA_POD_OPT_Float(&info->max_quantum),
			SPA_PARAM_LATENCY_minRate, SPA_POD_OPT_Int(&info->min_rate),
			SPA_PARAM_LATENCY_maxRate, SPA_POD_OPT_Int(&info->max_rate),
			SPA_PARAM_LATENCY_minNs, SPA_POD_OPT_Long(&info->min_ns),
			SPA_PARAM_LATENCY_maxNs, SPA_POD_OPT_Long(&info->max_ns))) < 0)
		return res;
	info->direction = (enum spa_direction)(info->direction & 1);
	return 0;
}

static inline struct spa_pod *
spa_latency_build(struct spa_pod_builder *builder, uint32_t id, const struct spa_latency_info *info)
{
	return (struct spa_pod *)spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_ParamLatency, id,
			SPA_PARAM_LATENCY_direction, SPA_POD_Id(info->direction),
			SPA_PARAM_LATENCY_minQuantum, SPA_POD_Float(info->min_quantum),
			SPA_PARAM_LATENCY_maxQuantum, SPA_POD_Float(info->max_quantum),
			SPA_PARAM_LATENCY_minRate, SPA_POD_Int(info->min_rate),
			SPA_PARAM_LATENCY_maxRate, SPA_POD_Int(info->max_rate),
			SPA_PARAM_LATENCY_minNs, SPA_POD_Long(info->min_ns),
			SPA_PARAM_LATENCY_maxNs, SPA_POD_Long(info->max_ns));
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_LATENCY_UTILS_H */
