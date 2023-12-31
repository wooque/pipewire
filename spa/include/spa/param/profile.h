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

#ifndef SPA_PARAM_PROFILE_H
#define SPA_PARAM_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#include <spa/param/param.h>

/** properties for SPA_TYPE_OBJECT_ParamProfile */
enum spa_param_profile {
	SPA_PARAM_PROFILE_START,
	SPA_PARAM_PROFILE_index,	/**< profile index (Int) */
	SPA_PARAM_PROFILE_name,		/**< profile name (String) */
	SPA_PARAM_PROFILE_description,	/**< profile description (String) */
	SPA_PARAM_PROFILE_priority,	/**< profile priority (Int) */
	SPA_PARAM_PROFILE_available,	/**< availability of the profile
					  *  (Id enum spa_param_availability) */
	SPA_PARAM_PROFILE_info,		/**< info (Struct(
					  *		  Int : n_items,
					  *		  (String : key,
					  *		   String : value)*)) */
	SPA_PARAM_PROFILE_classes,	/**< node classes provided by this profile
					  *  (Struct(
					  *	   Int : number of items following
					  *        Struct(
					  *           String : class name (eg. "Audio/Source"),
					  *           Int : number of nodes
					  *           String : property (eg. "card.profile.devices"),
					  *           Array of Int: device indexes
					  *         )*)) */
	SPA_PARAM_PROFILE_save,		/**< If profile should be saved (Bool) */
};

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_PROFILE_H */
