/*
 * Copyright (C) 2008-2010 Felipe Contreras
 * Copyright (C) 2012 matsi
 *
 * This file may be used under the terms of the GNU Lesser General Public
 * License version 2.1, a copy of which is found in LICENSE included in the
 * packaging of this file.
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <error.h>
#include <errno.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <xf86drmMode.h>
#include <xf86drm.h>
#include <libkms.h>

#include "drmsink.h"
#include "log.h"

#define ROUND_UP(num, scale) (((num) + ((scale) - 1)) & ~((scale) - 1))

static void *parent_class;

#ifndef GST_DISABLE_GST_DEBUG
GstDebugCategory *drm_debug;
#endif

enum {
	PROP_0,
	PROP_CONN,
	PROP_CRTC,
	PROP_MODE,
	PROP_FILE,
};

struct gst_drm_sink {
	GstBaseSink parent;

	bool enabled;

	struct kms_driver *drv;

	unsigned char *frame[2];
	struct kms_bo *bo[2];
	uint32_t fb[2];

	uint32_t current;

    drmModeCrtcPtr saved_crtc;
	drmModeModeInfo *mode;

	gchar *mode_name;
	gchar *device;

	uint32_t width;
	uint32_t height;

	uint32_t conn_id;
	uint32_t crtc_id;

	int fd;
};

struct gst_drm_sink_class {
	GstBaseSinkClass parent_class;
};

static GstCaps *
generate_sink_template(void)
{
	GstCaps *caps;
	GstStructure *struc;

	caps = gst_caps_new_empty();

	struc = gst_structure_new("video/x-raw-rgb",
			"width", GST_TYPE_INT_RANGE, 16, 4096,
			"height", GST_TYPE_INT_RANGE, 16, 4096,
			//"width", G_TYPE_INT, 1400,
			//"height", G_TYPE_INT, 1050,
			"bpp", G_TYPE_INT, 32,
			"depth", G_TYPE_INT, 24,
			"endianness", G_TYPE_INT, 4321,
			"green_mask", G_TYPE_INT, 16711680,
			"red_mask", G_TYPE_INT, 65280,
			"blue_mask", G_TYPE_INT, -16777216,
			"framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
			NULL);

	gst_caps_append_structure(caps, struc);

	return caps;
}

static gboolean
setup(struct gst_drm_sink *self, GstCaps *caps)
{
	GstStructure *structure;
	int width, height;
	int ret, i;

	uint32_t stride;
	uint32_t handle;

	uint32_t attr[7];/* = {
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_TERMINATE_PROP_LIST
	};*/

	/* */

	structure = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(structure, "width", &width);
	gst_structure_get_int(structure, "height", &height);

	//attr[1] = self->mode->hdisplay;
	//attr[3] = self->mode->vdisplay;

	//attr[1] = self->width = width;
	//attr[3] = self->height = height;

	/* configure drm buffers */

	for(i = 0; i < DRM_FRAMES; i++) {

	attr[0] = KMS_WIDTH;
	attr[1] = width;
	attr[2] = KMS_HEIGHT;
	attr[3] = height;
	attr[4] = KMS_BO_TYPE;
	attr[5] = KMS_BO_TYPE_SCANOUT_X8R8G8B8;
	attr[6] = KMS_TERMINATE_PROP_LIST;


		ret = kms_bo_create(self->drv, attr, &self->bo[i]);
		if (ret) {
			perror("failed kms_bo_create()");
			return false;
		}

		ret = kms_bo_get_prop(self->bo[i], KMS_PITCH, &stride);
		if (ret) {
			perror("failed kms_bo_get_prop(KMS_PITCH)");
			return false;
		}

		ret = kms_bo_get_prop(self->bo[i], KMS_HANDLE, &handle);
		if (ret) {
			perror("failed kms_bo_get_prop(KMS_HANDLE)");
			return false;
		}

		ret = kms_bo_map(self->bo[i], (void **) &self->frame[i]);
		if (ret) {
			perror("failed kms_bo_map()");
			return false;
		}

		ret = drmModeAddFB(self->fd, self->mode->hdisplay, self->mode->vdisplay, 24, 32, stride, handle, &self->fb[i]);
		//ret = drmModeAddFB(self->fd, self->mode->hdisplay, self->mode->vdisplay, 24, 32, stride, handle, &self->fb[i]);
		//ret = drmModeAddFB(self->fd, width, height, 24, 32, stride, handle, &self->fb[i]);
		if (ret) {
			perror("failed drmModeAddFB()");
			return false;
		}

		fprintf(stdout, "hdisplay = %d\n", self->mode->hdisplay);
		fprintf(stdout, "vdisplay = %d\n", self->mode->vdisplay);
		fprintf(stdout, "width = %d\n", width);
		fprintf(stdout, "height = %d\n", height);
		fprintf(stdout, "stride = %d\n", stride);
		fprintf(stdout, "handle = %d\n", handle);
	}

	/* store current crtc */

    self->saved_crtc = drmModeGetCrtc(self->fd, self->crtc_id);
    if (self->saved_crtc == NULL) {
		perror("failed drmModeGetCrtc(current)");
        return false;
    }

	self->enabled = true;
	self->current = 0;

	return true;
}

static gboolean
setcaps(GstBaseSink *base, GstCaps *caps)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	if (self->enabled)
		return true;

	return setup(self, caps);
}

static void
get_property (GObject * object, guint prop_id,
	GValue *value, GParamSpec *pspec)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *) object;

	switch (prop_id) {
		case PROP_CONN:
			g_value_set_int (value, self->conn_id);
			break;
		case PROP_CRTC:
			g_value_set_int (value, self->crtc_id);
			break;
		case PROP_MODE:
			g_value_set_string (value, self->mode_name);
			break;
		case PROP_FILE:
			g_value_set_string (value, self->device);
			break;
		default:
			break;
	}
}

static void
set_property (GObject * object, guint prop_id,
	const GValue *value, GParamSpec *pspec)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *) object;

	switch (prop_id) {
		case PROP_CONN:
			self->conn_id = g_value_get_int (value);
			break;
		case PROP_CRTC:
			self->crtc_id = g_value_get_int (value);
			break;
		case PROP_MODE:
			self->mode_name = g_strdup (g_value_get_string (value));
			break;
		case PROP_FILE:
			self->device = g_strdup (g_value_get_string (value));
			break;
		default:
			break;
	}
}

static gboolean
start(GstBaseSink *base)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	drmModeConnector *connector = NULL;

    drmModeRes *resources;
	drmModeModeInfo *mode;

	int ret, i;

	/* open drm device */

	self->fd = open(self->device, O_RDWR | O_CLOEXEC);
	if (self->fd < 0) {
		perror("cannot open drm device");
		return false;
	}

	/* get drm mode */

	resources = drmModeGetResources(self->fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed\n");
		return false;
	}

	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(self->fd, resources->connectors[i]);
		if (!connector)
			continue;

		if (connector->connector_id == self->conn_id)
			break;

		drmModeFreeConnector(connector);
	}

	if (i == resources->count_connectors) {
		fprintf(stderr, "No proper connector found\n");
		drmModeFreeResources(resources);
		return false;
	}

    for (i = 0, mode = connector->modes; i < connector->count_modes; i++, mode++) {
        if (0 == strcmp(mode->name, self->mode_name))
            break;
    }

	if (i == connector->count_modes) {
        fprintf(stderr, "No selected mode\n");
		drmModeFreeConnector(connector);
		return false;
    }

	drmModeFreeResources(resources);

	self->mode = mode;

	/* create libkms driver */

	ret = kms_create(self->fd, &self->drv);
	if (ret) {
		perror("failed kms_create()");
		return false;
	}

	fprintf(stdout, "connector  = %d\n", self->conn_id);
	fprintf(stdout, "crtc = %d\n", self->crtc_id);
	fprintf(stdout, "device = %s\n", self->device);
	fprintf(stdout, "mode = %s\n", self->mode->name);

	return true;
}

static gboolean
stop(GstBaseSink *base)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	int ret, i;

    if (self->saved_crtc->mode_valid) {
        ret = drmModeSetCrtc(self->fd, self->saved_crtc->crtc_id, self->saved_crtc->buffer_id,
                self->saved_crtc->x, self->saved_crtc->y, &self->conn_id, 1, &self->saved_crtc->mode);

        if (ret) {
            perror("failed drmModeSetCrtc(restore original)");
        }
    }

	for(i = 0; i < DRM_FRAMES; i++) {
		drmModeRmFB(self->fd, self->fb[i]);
		kms_bo_unmap(self->bo[i]);
		kms_bo_destroy(&self->bo[i]);
	}

	kms_destroy(&self->drv);

	close(self->fd);

	return true;
}

static GstFlowReturn
render(GstBaseSink *base, GstBuffer *buffer)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	uint32_t current = self->current;
	int ret, i, j;

#if 1
	memcpy(self->frame[current], GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
#else
	
#endif

    ret = drmModeSetCrtc(self->fd, self->crtc_id, self->fb[current], 0, 0,
            &self->conn_id, 1, self->mode);

	if (ret) {
		perror("failed drmModeSetCrtc(new)");
		return GST_FLOW_ERROR;
    }

    drmModeDirtyFB(self->fd, self->fb[current], NULL, 0);

	self->current = current ^ 1;

	return GST_FLOW_OK;
}

static void
class_init(void *g_class, void *class_data)
{
	GObjectClass *gobject_class;
	GstBaseSinkClass *base_sink_class;

	gobject_class = (GObjectClass *) g_class;
	base_sink_class = g_class;

	parent_class = g_type_class_ref(GST_DRM_SINK_TYPE);

	gobject_class->get_property = get_property;
	gobject_class->set_property = set_property;

	g_object_class_install_property (gobject_class, PROP_CONN,
			g_param_spec_int ("conn", "connector_id", "DRM connector id",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_CRTC,
			g_param_spec_int ("crtc", "crtc_id", "DRM crtc id",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_MODE,
			g_param_spec_string ("mode", "mode", "DRM connector mode",
				"preferred", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_FILE,
			g_param_spec_string ("device", "device", "DRM device",
				"/dev/dri/card0", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	base_sink_class->set_caps = setcaps;
	base_sink_class->start = start;
	base_sink_class->stop = stop;
	base_sink_class->render = render;
	base_sink_class->preroll = render;
}

static void
base_init(void *g_class)
{
	GstElementClass *element_class = g_class;
	GstPadTemplate *template;

	gst_element_class_set_details_simple(element_class,
			"Linux DRM sink",
			"Sink/Video",
			"Renders video with drm",
			"matsi");

	template = gst_pad_template_new("sink", GST_PAD_SINK,
			GST_PAD_ALWAYS,
			generate_sink_template());

	gst_element_class_add_pad_template(element_class, template);

	gst_object_unref(template);
}

GType
gst_drm_sink_get_type(void)
{
	static GType type;

	if (G_UNLIKELY(type == 0)) {
		GTypeInfo type_info = {
			.class_size = sizeof(struct gst_drm_sink_class),
			.class_init = class_init,
			.base_init = base_init,
			.instance_size = sizeof(struct gst_drm_sink),
		};

		type = g_type_register_static(GST_TYPE_BASE_SINK, "GstDrmSink", &type_info, 0);
	}

	return type;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
#ifndef GST_DISABLE_GST_DEBUG
	drm_debug = _gst_debug_category_new("drmsink", 0, "drmsink");
#endif

	if (!gst_element_register(plugin, "drmsink", GST_RANK_SECONDARY, GST_DRM_SINK_TYPE))
		return false;

	return true;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = GST_VERSION_MAJOR,
	.minor_version = GST_VERSION_MINOR,
	.name = "drmsink",
	.description = (gchar *) "Simple Linux DRM sink",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "LGPL",
	.source = "source",
	.package = "package",
	.origin = "origin",
};
