/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Gypsy
 *
 * A simple to use and understand GPSD replacement
 * that uses D-Bus, GLib and memory allocations
 *
 * Author: Iain Holmes <iain@gnome.org>
 * Copyright (C) 2007
 */

#ifndef __GYPSY_SATELLITE_H__
#define __GYPSY_SATELLITE_H__

#include <glib-object.h>

#include <gypsy/gypsy-device.h>

G_BEGIN_DECLS 

/**
 * GYPSY_SATELLITE_DBUS_SERVICE:
 *
 * A define containing the address of the Satellite service.
 */
#define GYPSY_SATELLITE_DBUS_SERVICE "org.freedesktop.Gypsy"

/** 
 * GYPSY_SATELLITE_DBUS_INTERFACE:
 * 
 * A define containing the name of the Satellite interface
 */
#define GYPSY_SATELLITE_DBUS_INTERFACE "org.freedesktop.Gypsy.Satellite"

#define GYPSY_TYPE_SATELLITE (gypsy_satellite_get_type ())
#define GYPSY_SATELLITE(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), GYPSY_TYPE_SATELLITE, GypsySatellite))
#define GYPSY_IS_SATELLITE(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), GYPSY_TYPE_SATELLITE))

/**
 * GypsySatelliteDetails:
 * @satellite_id: The satellite PRN id
 * @in_use: Whether this satellite was used in calculating the fix
 * @elevation: The satellite elevation
 * @azimuth: The satellite azimuth
 * @snr: The signal to noise ratio
 *
 * A structure defining a satellite
 */
typedef struct _GypsySatelliteDetails {
	int satellite_id;
	gboolean in_use;
	guint elevation;
	guint azimuth;
	guint snr;
} GypsySatelliteDetails;

/**
 * GypsySatellite:
 *
 * There are no public fields in #GypsySatellite.
 */
typedef struct _GypsySatellite {
	GObject parent_object;
} GypsySatellite;

typedef struct _GypsySatelliteClass {
	GObjectClass parent_class;

	void (*satellites_changed) (GypsySatellite *satellite,
				    GPtrArray   *satellites);
} GypsySatelliteClass;

GType gypsy_satellite_get_type (void);

GypsySatellite *gypsy_satellite_new (const char *object_path);

GPtrArray *gypsy_satellite_get_satellites (GypsySatellite *satellite,
					   GError        **error);
void gypsy_satellite_free_satellite_array (GPtrArray *satellites);

G_END_DECLS

#endif
