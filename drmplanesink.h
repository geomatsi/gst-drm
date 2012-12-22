/*
 * Copyright (C) 2008-2010 Felipe Contreras
 * Copyright (C) 2012 matsi
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#ifndef DRMPLANESINK_H
#define DRMPLANESINK_H

#include <glib-object.h>

#define GST_DRMPLANE_SINK_TYPE (gst_drmplane_sink_get_type())

GType gst_drmplane_sink_get_type(void);

#define DRM_FRAMES	2

#define DEFAULT_PROP_FILE	"/dev/dri/card0"

#endif /* DRMPLANESINK_H */
