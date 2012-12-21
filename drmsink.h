/*
 * Copyright (C) 2008-2010 Felipe Contreras
 * Copyright (C) 2012 matsi
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#ifndef DRMSINK_H
#define DRMSINK_H

#include <glib-object.h>

#define GST_DRM_SINK_TYPE (gst_drm_sink_get_type())

GType gst_drm_sink_get_type(void);

#define DRM_FRAMES	2

#endif /* DRMSINK_H */
