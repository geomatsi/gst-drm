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
	PROP_PLANE,
	PROP_CRTC,
	PROP_POSX,
	PROP_POSY,
	PROP_FILE,
};

struct gst_drm_sink {
	GstBaseSink parent;

	bool enabled;

	struct kms_driver *drv;

	unsigned char *frame[2];
	struct kms_bo *bo[2];
	uint32_t stride[2];
	uint32_t fb[2];

	uint32_t current;

	gchar *device;

	uint32_t width;
	uint32_t height;
	uint32_t posx;
	uint32_t posy;

	uint32_t plane_id;
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

	uint32_t attr[] = {
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_TERMINATE_PROP_LIST
	};

	/* */

	structure = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(structure, "width", &width);
	gst_structure_get_int(structure, "height", &height);

	attr[1] = self->width = width;
	attr[3] = self->height = height;

	/* configure drm buffers */

	for(i = 0; i < DRM_FRAMES; i++) {

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

		self->stride[i] = stride;

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

		ret = drmModeAddFB(self->fd, self->width, self->height, 24, 32, stride, handle, &self->fb[i]);
		if (ret) {
			perror("failed drmModeAddFB()");
			return false;
		}
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
		case PROP_PLANE:
			g_value_set_int (value, self->plane_id);
			break;
		case PROP_CRTC:
			g_value_set_int (value, self->crtc_id);
			break;
		case PROP_POSX:
			g_value_set_int (value, self->posx);
			break;
		case PROP_POSY:
			g_value_set_int (value, self->posy);
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
		case PROP_PLANE:
			self->plane_id = g_value_get_int (value);
			break;
		case PROP_CRTC:
			self->crtc_id = g_value_get_int (value);
			break;
		case PROP_POSX:
			self->posx = g_value_get_int (value);
			break;
		case PROP_POSY:
			self->posy = g_value_get_int (value);
			break;
		case PROP_FILE:
			self->device = g_strdup (g_value_get_string (value));
			if (self->device == NULL) {
				self->device = g_strdup(DEFAULT_PROP_FILE);
			}
			break;
		default:
			break;
	}
}

static gboolean
start(GstBaseSink *base)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	drmModePlane *plane = NULL;
	drmModePlaneRes *resources;
	uint32_t i;
	int ret;

	/* open drm device */

	self->fd = open(self->device, O_RDWR | O_CLOEXEC);
	if (self->fd < 0) {
		perror("cannot open drm device");
		return false;
	}

	/* check drm plane */

	resources = drmModeGetPlaneResources(self->fd);
	if (!resources || resources->count_planes == 0) {
		fprintf(stderr, "drmModeGetPlaneResources failed\n");
		return false;
	}

	for (i = 0; i < resources->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(self->fd, resources->planes[i]);
		if (!p)
			continue;

		if (p->plane_id == self->plane_id) {
			plane = p;
			break;
		}

		drmModeFreePlane(plane);
	}

	if (!plane) {
		fprintf(stderr, "couldn't find specified plane\n");
		return false;
	}

	/* create libkms driver */

	ret = kms_create(self->fd, &self->drv);
	if (ret) {
		perror("failed kms_create()");
		return false;
	}

	return true;
}

static gboolean
stop(GstBaseSink *base)
{
	struct gst_drm_sink *self = (struct gst_drm_sink *)base;
	int i;

	drmModeSetPlane(self->fd, self->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

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
	int ret;

	if (GST_BUFFER_SIZE(buffer) < 4 * self->width * self->height) {
		fprintf(stderr, "Incoming buffer is less than expected. Some negotiation problem occured...\n");
		return GST_FLOW_ERROR;
	}

	{
		uint8_t *dst = (uint8_t *) self->frame[current];
		uint8_t *src = (uint8_t *) GST_BUFFER_DATA(buffer);
		uint32_t s;

		for (s = 0; s < self->height; s++) {
			memcpy(dst + s*self->stride[current], src + 4 * s * self->width, 4 * self->width);
		}
	}

	ret = drmModeSetPlane(self->fd, self->plane_id, self->crtc_id, self->fb[current], 0,
		self->posx, self->posy, self->width, self->height, 0, 0, self->width << 16, self->height << 16);

	if (ret) {
		fprintf(stderr, "cannot set plane\n");
		return GST_FLOW_ERROR;
	}

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

	g_object_class_install_property (gobject_class, PROP_PLANE,
			g_param_spec_int ("plane", "plane_id", "DRM plane id",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_CRTC,
			g_param_spec_int ("crtc", "crtc_id", "DRM crtc id",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_POSX,
			g_param_spec_int ("posx", "posx", "plane left top corner X position",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_POSY,
			g_param_spec_int ("posy", "posy", "plane left top corner Y position",
				0, 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_FILE,
			g_param_spec_string ("device", "device", "DRM device",
				DEFAULT_PROP_FILE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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
			"Linux DRM plane sink",
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

	if (!gst_element_register(plugin, "drmplanesink", GST_RANK_SECONDARY, GST_DRM_SINK_TYPE))
		return false;

	return true;
}

GstPluginDesc gst_plugin_desc = {
	.major_version = GST_VERSION_MAJOR,
	.minor_version = GST_VERSION_MINOR,
	.name = "drmplanesink",
	.description = (gchar *) "Simple Linux DRM plane sink",
	.plugin_init = plugin_init,
	.version = VERSION,
	.license = "LGPL",
	.source = "source",
	.package = "package",
	.origin = "origin",
};
