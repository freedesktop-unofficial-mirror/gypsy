/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Gypsy
 *
 * A simple to use and understand GPSD replacement
 * that uses D-Bus, GLib and memory allocations
 *
 * Author: Iain Holmes <iain@gnome.org>
 * Copyright (C) 2007 Iain Holmes
 * Copyright (C) 2007 Openedhand Ltd
 */

/*
 * GypsyClient - The GPS connection object that control GPS devices.
 */

#include <config.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef HAVE_BLUEZ
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#endif

#include <glib.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include "gypsy-client.h"
#include "gypsy-marshal-internal.h"
#include "nmea-parser.h"

#define GYPSY_ERROR g_quark_from_static_string ("gypsy-error")

/* Defined in main.c */
extern char* nmea_log;

#define READ_BUFFER_SIZE 1024
typedef struct _GypsyClientPrivate {
	char *device_path; /* Device path of our GPS */
	int fd; /* File descriptor used to read from the GPS */

	GIOChannel *channel; /* The channel we talk to the GPS on */
	GIOChannel *debug_log; /* The channel to write the NMEA to, or NULL if
				  debugging is off */
	guint32 error_id, connect_id, input_id;

	char sentence[READ_BUFFER_SIZE + 1]; /* This is for building the 
						NMEA sentence */
	int chars_in_buffer; /* How many characters are in the buffer */

	NMEAParseContext *ctxt;

	/* Fix details */
	int timestamp;
	FixType fix_type;

	/* Position details */
	PositionFields position_fields;
	double latitude;
	double longitude;
	double altitude;

	/* For calculating climb */
	int last_alt_timestamp;

	/* Accuracy details */
	AccuracyFields accuracy_fields;
	double pdop;
	double hdop;
	double vdop;
	
	/* Course details */
	CourseFields course_fields;
	double speed;
	double direction;
	double climb;

	/* The confirmed satellites */
	int sat_count; /* Number of satellites */
	GypsyClientSatellite satellites[MAX_SATELLITES];

	int new_sat_count; /* Number of satellites */ 
	GypsyClientSatellite new_satellites[MAX_SATELLITES];
} GypsyClientPrivate;

enum {
	PROP_0,
	PROP_DEVICE,
};

enum {
	ACCURACY_CHANGED,
	POSITION_CHANGED,
	COURSE_CHANGED,
	SATELLITES_CHANGED,
	CONNECTION_CHANGED,
	FIX_STATUS,
	TIME_CHANGED,
	LAST_SIGNAL
};

static guint32 signals[LAST_SIGNAL] = {0, };

G_DEFINE_TYPE (GypsyClient, gypsy_client, G_TYPE_OBJECT);

#define GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GYPSY_TYPE_CLIENT, GypsyClientPrivate))
#define GYPSY_CLIENT_SATELLITES_CHANGED_TYPE (dbus_g_type_get_struct ("GValueArray", G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INVALID))

static gboolean gypsy_client_start (GypsyClient *client,
				    GError     **error);
static gboolean gypsy_client_stop (GypsyClient *client,
				   GError     **error);
static gboolean gypsy_client_get_fix_status (GypsyClient *client,
					     int         *fix_status,
					     GError     **error);
static gboolean gypsy_client_get_connection_status (GypsyClient *client,
						    gboolean    *connected,
						    GError     **error);
static gboolean gypsy_client_get_accuracy (GypsyClient *client,
					   int         *fields_OUT,
					   double *pdop_OUT,
					   double *hdop_OUT,
					   double *vdop_OUT,
					   GError **error);
static gboolean gypsy_client_get_position (GypsyClient *client,
					   int         *fields_OUT,
					   int         *timestamp_OUT,
					   double      *latitude_OUT,
					   double      *longitude_OUT,
					   double      *altitude_OUT,
					   GError     **error);
static gboolean gypsy_client_get_course (GypsyClient *client,
					 int         *fields_OUT,
					 int         *timestamp_OUT,
					 double      *speed_OUT,
					 double      *direction_OUT,
					 double      *climb_OUT,
					 GError     **error);
static gboolean gypsy_client_get_satellites (GypsyClient *client,
					     GPtrArray  **satellites_OUT,
					     GError     **error);
static gboolean gypsy_client_get_time (GypsyClient *client,
				       int         *timestamp_OUT,
				       GError     **error);
#include "gypsy-client-glue.h"

static void
shutdown_connection (GypsyClient *client)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	if (priv->error_id > 0) {
		g_source_remove (priv->error_id);
		priv->error_id = 0;
	}

	if (priv->connect_id > 0) {
		g_source_remove (priv->connect_id);
		priv->connect_id = 0;
	}

	if (priv->input_id > 0) {
		g_source_remove (priv->input_id);
		priv->input_id = 0;
	}
		
	if (priv->fd > 0) {
		close (priv->fd);
		priv->fd = -1;
	}

	if (priv->channel) {
		g_io_channel_unref (priv->channel);
		priv->channel = NULL;
	}

	if (priv->debug_log) {
		g_io_channel_unref (priv->debug_log);
		priv->debug_log = NULL;
	}

	priv->chars_in_buffer = 0;
}

static gboolean
gps_channel_error (GIOChannel  *channel,
		   GIOCondition condition,
		   gpointer     userdata)
{
	GypsyClient *client;
	GypsyClientPrivate *priv;

	client = (GypsyClient *) userdata;
	priv = GET_PRIVATE (userdata);

	g_debug ("Error on connection to %s", priv->device_path);
	shutdown_connection ((GypsyClient *) userdata);

	g_signal_emit (G_OBJECT (client), signals[CONNECTION_CHANGED],
		       0, FALSE);

	return FALSE;
}

static gboolean
gps_channel_input (GIOChannel  *channel,
		   GIOCondition condition,
		   gpointer     userdata)
{
	GypsyClientPrivate *priv;
	GIOStatus status;
	char *buf;
	gsize chars_left_in_buffer, chars_read;
	GError *error = NULL;

	priv = GET_PRIVATE (userdata);

	buf = priv->sentence + priv->chars_in_buffer;
	chars_left_in_buffer = READ_BUFFER_SIZE - priv->chars_in_buffer;

	status = g_io_channel_read_chars (priv->channel, buf, 
					  chars_left_in_buffer,
					  &chars_read, &error);

	if (priv->debug_log) {
		g_io_channel_write_chars
			(priv->debug_log, buf, chars_read, NULL, NULL);
	}

	if (status == G_IO_STATUS_NORMAL) {
		char *eos = NULL;
		int length = 0;
		priv->chars_in_buffer += chars_read;

		/* Append a \0 to treat as a string, the \0 will be overwritten
		   in the next call to g_io_channel_read_chars */
 		*(priv->sentence + priv->chars_in_buffer) = '\0';

		/* g_debug ("Buffer: %s", priv->sentence);  */
		
		/* NMEA sentences end with <LF> 
		   Get <LF> at the end of the sentence */
		while ((eos = strchr (priv->sentence, '\n'))) {
			length = (eos - priv->sentence) + 1;

			if (length > 1) {
				/* Remove the <LF> */
				*eos = '\0';
				
				if (nmea_parse_sentence (priv->ctxt,
							 priv->sentence, 
							 NULL) == FALSE) {
					g_debug ("Invalid sentence: %s.", priv->sentence);
				}
			}
			
			/* Only need to remove sentence is there was something 
			   in the first place */
			if (length > 0) {
				/* Remove the sentence from the builder and
				   move the rest up including terminating 0 */
				memmove (priv->sentence, eos + 1, 
					 (priv->chars_in_buffer - length) + 1);
				priv->chars_in_buffer -= length;
			}
		}
	}
	
	return TRUE;
}

static gboolean
gps_channel_connect (GIOChannel  *channel,
		     GIOCondition condition,
		     gpointer     userdata)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (userdata);

	g_debug ("GPS channel can connect");

	priv->input_id = g_io_add_watch_full (priv->channel, 
					      G_PRIORITY_HIGH_IDLE,
					      G_IO_IN | G_IO_PRI,
					      gps_channel_input, userdata, 
					      NULL);

	g_signal_emit (G_OBJECT (userdata), signals[CONNECTION_CHANGED],
		       0, TRUE);

	priv->connect_id = 0;
	return FALSE;
}

static gboolean
gypsy_client_start (GypsyClient *client,
		    GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	g_debug ("Starting connection to %s", priv->device_path);

	if (priv->fd != -1) {
		g_debug ("Connection to %s already started", priv->device_path);
		return TRUE;
	}

	/* Open a connection to our device */
	if (priv->device_path[0] == '/') {
		struct termios term;

		priv->fd = open (priv->device_path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
		
		if (tcgetattr (priv->fd, &term) < 0) {
			g_warning ("Error getting term");
			g_set_error (error, GYPSY_ERROR, errno, 
				     g_strerror (errno));
			return FALSE;
		}
		
		/* According to NMEA-0183 GPS serial configuration is:
		 * Baud Rate: 4800 bps
		 * Data bits: 8
		 * Parity:    none
		 * Stop Bits: 1 (or more)
		 * Handshake: none
		 */
		term.c_cflag &= ~(PARENB | PARODD | CRTSCTS);
		term.c_cflag |= CREAD | CLOCAL;

		term.c_iflag &= ~(PARMRK | INPCK);
		term.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
		term.c_cflag |= CS8;

		cfsetspeed (&term, B4800);

		if (tcsetattr (priv->fd, TCSANOW, &term) < 0) {
			g_warning ("Error setting term");
			g_set_error (error, GYPSY_ERROR, errno, 
				     g_strerror (errno));
			return FALSE;
		}
	} else {
#ifdef HAVE_BLUEZ
		priv->fd = socket (AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
#else
		g_warning ("Trying to connect to a Bluetooth GPS but Gypsy does not have Bluetooth support");
		return FALSE;
#endif
	}

	if (priv->fd == -1) {
		/* Error */
		g_set_error (error, GYPSY_ERROR, errno, g_strerror (errno));
		return FALSE;
	}

	if (nmea_log) {
		char *device, *filename;
		device = g_path_get_basename (priv->device_path);
		filename = g_strconcat (nmea_log, ".", device, NULL);
		priv->debug_log = g_io_channel_new_file (filename, "w", NULL);
		g_io_channel_set_encoding (priv->debug_log, NULL, NULL);
		g_io_channel_set_buffered (priv->debug_log, FALSE);
		g_free (device);
		g_free (filename);
	}

	priv->channel = g_io_channel_unix_new (priv->fd);
	g_io_channel_set_flags (priv->channel, G_IO_FLAG_NONBLOCK, NULL);
	priv->error_id = g_io_add_watch_full (priv->channel, 
					      G_PRIORITY_HIGH_IDLE,
					      G_IO_ERR | G_IO_HUP,
					      gps_channel_error, client, NULL);
	priv->connect_id = g_io_add_watch_full (priv->channel,
						G_PRIORITY_HIGH_IDLE,
						G_IO_OUT, gps_channel_connect,
						client, NULL);

#ifdef HAVE_BLUEZ
	/* Now connect to the bluetooth socket */
	if (priv->device_path[0] != '/') {
		struct sockaddr_rc addr = { 0 };

		addr.rc_family = AF_BLUETOOTH;
		addr.rc_channel = (uint8_t) 1;
		str2ba(priv->device_path, &addr.rc_bdaddr);

		if (connect (priv->fd, (struct sockaddr *) &addr,
			     sizeof (addr)) == -1) {
			/* Error */
			if (errno == EINPROGRESS || errno == EAGAIN) {
				return TRUE;
			}

			g_warning ("Error connecting: %s", g_strerror (errno));
			g_set_error (error, GYPSY_ERROR, errno, g_strerror (errno));

			g_source_remove (priv->error_id);
			priv->error_id = 0;
			g_source_remove (priv->connect_id);
			priv->connect_id = 0;

			g_io_channel_unref (priv->channel);			
			priv->channel = NULL;

			close (priv->fd);
			priv->fd = -1;

			return FALSE;
		}
	}
#endif
	return TRUE;
}

static gboolean
gypsy_client_stop (GypsyClient *client,
		   GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	g_debug ("Stopping connection to %s", priv->device_path);
	shutdown_connection (client);

	g_signal_emit (G_OBJECT (client), signals[CONNECTION_CHANGED],
		       0, FALSE);
	return TRUE;
}

static gboolean
gypsy_client_get_connection_status (GypsyClient *client,
				    gboolean    *connected,
				    GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*connected = (priv->fd > 0);

	return TRUE;
}

static gboolean
gypsy_client_get_fix_status (GypsyClient *client,
			     int         *fix_status,
			     GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*fix_status = priv->fix_type;

	return TRUE;
}

static gboolean 
gypsy_client_get_accuracy (GypsyClient *client,
			   int         *fields_OUT,
			   double      *pdop_OUT,
			   double      *hdop_OUT,
			   double      *vdop_OUT,
			   GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*fields_OUT = priv->accuracy_fields;
	*pdop_OUT = priv->pdop;
	*hdop_OUT = priv->hdop;
	*vdop_OUT = priv->vdop;

	return TRUE;
}

static gboolean 
gypsy_client_get_position (GypsyClient *client,
			   int         *fields_OUT,
			   int         *timestamp_OUT,
			   double      *latitude_OUT,
			   double      *longitude_OUT,
			   double      *altitude_OUT,
			   GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*fields_OUT = priv->position_fields;
	*timestamp_OUT = priv->timestamp;
	*latitude_OUT = priv->latitude;
	*longitude_OUT = priv->longitude;
	*altitude_OUT = priv->altitude;

	return TRUE;
}

static gboolean 
gypsy_client_get_course (GypsyClient *client,
			 int         *fields_OUT,
			 int         *timestamp_OUT,
			 double      *speed_OUT,
			 double      *direction_OUT,
			 double      *climb_OUT,
			 GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*fields_OUT = priv->position_fields;
	*timestamp_OUT = priv->timestamp;
	*speed_OUT = priv->speed;
	*direction_OUT = priv->direction;
	*climb_OUT = priv->climb;

	return TRUE;
}

static gboolean
gypsy_client_get_satellites (GypsyClient *client,
			     GPtrArray  **satellites_OUT,
			     GError     **error)
{
	GypsyClientPrivate *priv;
	GPtrArray *sat_array;
	int i;

	priv = GET_PRIVATE (client);

	sat_array = g_ptr_array_new ();
	
	for (i = 0; i < priv->sat_count; i++) {
		GValue sat_struct = {0, };
		GypsyClientSatellite *sat;
		
		sat = &priv->satellites[i];
		
		g_value_init (&sat_struct,
			      GYPSY_CLIENT_SATELLITES_CHANGED_TYPE);
		g_value_take_boxed (&sat_struct,
				    dbus_g_type_specialized_construct
				    (GYPSY_CLIENT_SATELLITES_CHANGED_TYPE));
		
		dbus_g_type_struct_set (&sat_struct,
					0, sat->satellite_id,
					1, sat->in_use,
					2, sat->elevation,
					3, sat->azimuth,
					4, sat->snr,
					G_MAXUINT);
		g_ptr_array_add (sat_array, 
				 g_value_get_boxed (&sat_struct));
	}

	*satellites_OUT = sat_array;
	
	return TRUE;
}

static gboolean
gypsy_client_get_time (GypsyClient *client,
		       int         *timestamp_OUT,
		       GError     **error)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	*timestamp_OUT = priv->timestamp;

	return TRUE;
}

static void
finalize (GObject *object) 
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (object);

	shutdown_connection ((GypsyClient *) object);

	g_free (priv->device_path);
	g_free (priv->ctxt);

	((GObjectClass *) gypsy_client_parent_class)->finalize (object);
}

static void
dispose (GObject *object)
{
	((GObjectClass *) gypsy_client_parent_class)->dispose (object);
}

static void
set_property (GObject      *object,
	      guint         prop_id,
	      const GValue *value,
	      GParamSpec   *pspec)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_DEVICE:
		priv->device_path = g_value_dup_string (value);
		
		break;

	default:
		break;
	}
}

static void
get_property (GObject    *object,
	      guint       prop_id,
	      GValue     *value,
	      GParamSpec *pspec)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_DEVICE:
		g_value_set_string (value, priv->device_path);
		break;

	default:
		break;
	}
}

static void
gypsy_client_class_init (GypsyClientClass *klass)
{
	GObjectClass *o_class;

	o_class = (GObjectClass *) klass;

	o_class->finalize = finalize;
	o_class->dispose = dispose;
	o_class->set_property = set_property;
	o_class->get_property = get_property;

	g_type_class_add_private (klass, sizeof (GypsyClientPrivate));

	g_object_class_install_property (o_class,
					 PROP_DEVICE,
					 g_param_spec_string ("device_path",
							      "Device path", 
							      "The path of the GPS device",
							      "", 
							      G_PARAM_WRITABLE |
							      G_PARAM_CONSTRUCT_ONLY));

	signals[ACCURACY_CHANGED] = g_signal_new ("accuracy-changed",
						  G_TYPE_FROM_CLASS (klass),
						  G_SIGNAL_RUN_FIRST |
						  G_SIGNAL_NO_RECURSE,
						  G_STRUCT_OFFSET (GypsyClientClass, 
								   accuracy_changed),
						  NULL, NULL,
						  gypsy_marshal_VOID__INT_DOUBLE_DOUBLE_DOUBLE,
						  G_TYPE_NONE,
						  4, G_TYPE_INT,
						  G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
	signals[POSITION_CHANGED] = g_signal_new ("position-changed",
						  G_TYPE_FROM_CLASS (klass),
						  G_SIGNAL_RUN_FIRST |
						  G_SIGNAL_NO_RECURSE,
						  G_STRUCT_OFFSET (GypsyClientClass, 
								   position_changed),
						  NULL, NULL,
						  gypsy_marshal_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE,
						  G_TYPE_NONE,
						  5, G_TYPE_INT,
						  G_TYPE_INT, G_TYPE_DOUBLE,
						  G_TYPE_DOUBLE, G_TYPE_DOUBLE);
	signals[COURSE_CHANGED] = g_signal_new ("course-changed",
						G_TYPE_FROM_CLASS (klass),
						G_SIGNAL_RUN_FIRST |
						G_SIGNAL_NO_RECURSE,
						G_STRUCT_OFFSET (GypsyClientClass, 
								 course_changed),
						NULL, NULL,
						gypsy_marshal_VOID__INT_INT_DOUBLE_DOUBLE_DOUBLE,
						G_TYPE_NONE,
						5, G_TYPE_INT,
						G_TYPE_INT, G_TYPE_DOUBLE,
						G_TYPE_DOUBLE, G_TYPE_DOUBLE);
	signals[SATELLITES_CHANGED] = g_signal_new ("satellites-changed",
						    G_TYPE_FROM_CLASS (klass),
						    G_SIGNAL_RUN_LAST, 0,
						    NULL, NULL,
						    g_cclosure_marshal_VOID__BOXED,
						    G_TYPE_NONE, 1, 
						    (dbus_g_type_get_collection
						     ("GPtrArray", 
						      (dbus_g_type_get_struct
						       ("GValueArray", 
							G_TYPE_UINT, 
							G_TYPE_BOOLEAN,
							G_TYPE_UINT,
							G_TYPE_UINT,
							G_TYPE_UINT,
							G_TYPE_INVALID)))));
						    
	signals[CONNECTION_CHANGED] = g_signal_new ("connection-status-changed",
						    G_TYPE_FROM_CLASS (klass),
						    G_SIGNAL_RUN_FIRST |
						    G_SIGNAL_NO_RECURSE,
						    G_STRUCT_OFFSET (GypsyClientClass, connection_changed),
						    NULL, NULL,
						    g_cclosure_marshal_VOID__BOOLEAN,
						    G_TYPE_NONE,
						    1, G_TYPE_BOOLEAN);
	signals[FIX_STATUS] = g_signal_new ("fix-status-changed",
					    G_TYPE_FROM_CLASS (klass),
					    G_SIGNAL_RUN_FIRST |
					    G_SIGNAL_NO_RECURSE,
					    G_STRUCT_OFFSET (GypsyClientClass,
							     fix_status_changed),
					    NULL, NULL,
					    g_cclosure_marshal_VOID__INT,
					    G_TYPE_NONE, 
					    1, G_TYPE_INT);
	signals[TIME_CHANGED] = g_signal_new ("time-changed",
					      G_TYPE_FROM_CLASS (klass),
					      G_SIGNAL_RUN_FIRST |
					      G_SIGNAL_NO_RECURSE,
					      G_STRUCT_OFFSET (GypsyClientClass,
							       time_changed),
					      NULL, NULL,
					      g_cclosure_marshal_VOID__INT,
					      G_TYPE_NONE,
					      1, G_TYPE_INT);

	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass),
					 &dbus_glib_gypsy_client_object_info);
}

static void
gypsy_client_init (GypsyClient *client)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);
	priv->fd = -1;
	priv->ctxt = nmea_parse_context_new (client);
	priv->timestamp = 0;
	priv->last_alt_timestamp = 0;
}

void
gypsy_client_set_position (GypsyClient   *client,
			   PositionFields fields_set,
			   float          latitude,
			   float          longitude,
			   float          altitude)
{
	GypsyClientPrivate *priv;
	gboolean changed = FALSE;
	
	priv = GET_PRIVATE (client);

	if (fields_set & POSITION_LATITUDE) {
		if (priv->position_fields & POSITION_LATITUDE) {
			if (priv->latitude != latitude) {
				priv->latitude = latitude;
				changed = TRUE;
			}
		} else {
			priv->latitude = latitude;
			priv->position_fields |= POSITION_LATITUDE;
			changed = TRUE;
		}
	}

	if (fields_set & POSITION_LONGITUDE) {
		if (priv->position_fields & POSITION_LONGITUDE) {
			if (priv->longitude != longitude) {
				priv->longitude = longitude;
				changed = TRUE;
			}
		} else {
				priv->longitude = longitude;
				priv->position_fields |= POSITION_LONGITUDE;
				changed = TRUE;
		}
	}

	if (fields_set & POSITION_ALTITUDE) {
		if (priv->position_fields & POSITION_ALTITUDE) {
			if (priv->altitude != altitude) {
				/* If we've got a timestamp for the last alt
				   then we are able to calculate the climb. */
				if (priv->last_alt_timestamp > 0) {
					int dt;
					double da, climb;

					dt = priv->timestamp - priv->last_alt_timestamp;
					da = altitude - priv->altitude;
					climb = da / (double) dt;
					gypsy_client_set_course (client,
								 COURSE_CLIMB,
								 0.0, 0.0, 
								 climb);
				}

				priv->altitude = altitude;
				priv->last_alt_timestamp = priv->timestamp;
				changed = TRUE;
			} 
		} else {
			priv->altitude = altitude;
			priv->last_alt_timestamp = priv->timestamp;
			priv->position_fields |= POSITION_ALTITUDE;
			changed = TRUE;
		}
	}

 	if (changed) {
		g_signal_emit (client, signals[POSITION_CHANGED], 0,
			       priv->position_fields, priv->timestamp, 
			       priv->latitude, priv->longitude, priv->altitude);
	}
}

void
gypsy_client_set_course (GypsyClient *client,
			 CourseFields fields_set,
			 float        speed,
			 float        direction,
			 float        climb)
{
	GypsyClientPrivate *priv;
	gboolean changed = FALSE;
	
	priv = GET_PRIVATE (client);

	if (fields_set & COURSE_SPEED) {
		if (priv->course_fields & COURSE_SPEED) {
			if (priv->speed != speed) {
				priv->speed = speed;
				changed = TRUE;
			}
		} else {
			priv->speed = speed;
			priv->course_fields |= COURSE_SPEED;
			changed = TRUE;
		}
	}

	if (fields_set & COURSE_DIRECTION) {
		if (priv->course_fields & COURSE_DIRECTION) {
			if (priv->direction != direction) {
				priv->direction = direction;
				changed = TRUE;
			}
		} else {
			priv->direction = direction;
			priv->course_fields |= COURSE_DIRECTION;
			changed = TRUE;
		}
	}

	if (fields_set & COURSE_CLIMB) {
		if (priv->course_fields & COURSE_CLIMB) {
			if (priv->climb != climb) {
				priv->climb = climb;
				changed = TRUE;
			}
		} else {
			priv->climb = climb;
			priv->course_fields |= COURSE_CLIMB;
			changed = TRUE;
		}
	}

	if (changed) {
		/* Emit course changed, we don't deal with climb yet */
		g_signal_emit (client, signals[COURSE_CHANGED], 0,
			       priv->course_fields, priv->timestamp, 
			       priv->speed, priv->direction, priv->climb);
	}
}

void
gypsy_client_set_timestamp (GypsyClient *client,
			    int          utc_time)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	if (priv->timestamp != utc_time) {
		priv->timestamp = utc_time;
		g_signal_emit (client, signals[TIME_CHANGED], 0, utc_time);
	}
}

void
gypsy_client_set_fix_type (GypsyClient *client,
			   FixType      type,
			   gboolean     weak)
{
	GypsyClientPrivate *priv;
	FixType weak_type;

	priv = GET_PRIVATE (client);

	/* If the passed in type is "weak" (in an RMC sentence we only 
	   know if we have a fix or not) then we don't want to demote a 3D 
	   fix down to a 2D fix only to have it promoted 2 sentences later.
	   So we convert a 3D fix to a 2D one before checking */
	if (weak && priv->fix_type == FIX_3D) {
		weak_type = FIX_2D;
	} else {
		weak_type = priv->fix_type;
	}

	if (weak_type != type) {
		priv->fix_type = type;
		g_signal_emit (G_OBJECT (client), signals[FIX_STATUS], 0, type);
	}
}

void
gypsy_client_set_accuracy (GypsyClient *client,
			   AccuracyFields fields_set,
			   double pdop,
			   double hdop,
			   double vdop)
{
	GypsyClientPrivate *priv;
	gboolean changed = FALSE;
	
	priv = GET_PRIVATE (client);
	
	if (fields_set & ACCURACY_POSITION) {
		if (priv->accuracy_fields & ACCURACY_POSITION) {
			if (priv->pdop != pdop) {
				priv->pdop = pdop;
				changed = TRUE;
			}
		} else {
			priv->pdop = pdop;
			priv->accuracy_fields |= ACCURACY_POSITION;
			changed = TRUE;
		}
	}

	if (fields_set & ACCURACY_HORIZONAL) {
		if (priv->accuracy_fields & ACCURACY_HORIZONAL) {
			if (priv->hdop != hdop) {
				priv->hdop = hdop;
				changed = TRUE;
			}
		} else {
			priv->hdop = hdop;
			priv->accuracy_fields |= ACCURACY_HORIZONAL;
			changed = TRUE;
		}
	}

	if (fields_set & ACCURACY_VERTICAL) {
		if (priv->accuracy_fields & ACCURACY_VERTICAL) {
			if (priv->vdop != vdop) {
				priv->vdop = vdop;
				changed = TRUE;
			}
		} else {
			priv->vdop = vdop;
			priv->accuracy_fields |= ACCURACY_VERTICAL;
			changed = TRUE;
		}
	}

	if (changed) {
		g_signal_emit (client, signals[ACCURACY_CHANGED], 0,
			       priv->accuracy_fields,
			       priv->pdop, priv->hdop, priv->vdop);
	}
}

/* This adds a satellite to the new set of satellites.
   Once all the satellites are set, call gypsy_client_set_satellites
   to commit them */
void 
gypsy_client_add_satellite (GypsyClient *client,
			    int          satellite_id,
			    gboolean     in_use,
			    int          elevation,
			    int          azimuth,
			    int          snr)
{
	GypsyClientPrivate *priv;
	GypsyClientSatellite *satellite;

	priv = GET_PRIVATE (client);

	satellite = &(priv->new_satellites[priv->new_sat_count]);
	satellite->satellite_id = satellite_id;
	satellite->in_use = in_use;
	satellite->elevation = elevation;
	satellite->azimuth = azimuth;
	satellite->snr = snr;

	priv->new_sat_count++;
}

/* This is called if there was an error in the satellites
   like a missing message. If this happens then we just clear the ones
   we have and continue */
void 
gypsy_client_clear_satellites (GypsyClient *client)
{
	GypsyClientPrivate *priv;

	priv = GET_PRIVATE (client);

	priv->new_sat_count = 0;
}

/* Checks if the satellite details have changed, and if so copies the new
   set over the old and emits a signal */
void
gypsy_client_set_satellites (GypsyClient *client) 
{
	GypsyClientPrivate *priv;
	gboolean changed = FALSE;
	int i;

	priv = GET_PRIVATE (client);

	for (i = 0; i < priv->new_sat_count; i++) {
		GypsyClientSatellite *n, *o;

		n = &priv->new_satellites[i];
		o = &priv->satellites[i];

		if (n->satellite_id != o->satellite_id ||
		    n->in_use != o->in_use ||
		    n->elevation != o->elevation ||
		    n->azimuth != o->azimuth ||
		    n->snr != o->snr) {
			changed = TRUE;
			o->satellite_id = n->satellite_id;
			o->in_use = n->in_use;
			o->elevation = n->elevation;
			o->azimuth = n->azimuth;
			o->snr = n->snr;
		}
	}

	if (priv->new_sat_count != priv->sat_count) {
		changed = TRUE;
		priv->sat_count = priv->new_sat_count;
	}

	if (changed) {
		GPtrArray *sat_array;
		int i;

		sat_array = g_ptr_array_new ();
		for (i = 0; i < priv->sat_count; i++) {
			GValue sat_struct = {0, };
			GypsyClientSatellite *sat;

			sat = &priv->satellites[i];

			g_value_init (&sat_struct,
				      GYPSY_CLIENT_SATELLITES_CHANGED_TYPE);
			g_value_take_boxed (&sat_struct,
					    dbus_g_type_specialized_construct
					    (GYPSY_CLIENT_SATELLITES_CHANGED_TYPE));

			dbus_g_type_struct_set (&sat_struct,
						0, sat->satellite_id,
						1, sat->in_use,
						2, sat->elevation,
						3, sat->azimuth,
						4, sat->snr,
						G_MAXUINT);
			g_ptr_array_add (sat_array, 
					 g_value_get_boxed (&sat_struct));
		}

		g_signal_emit (client, signals[SATELLITES_CHANGED], 0, 
			       sat_array);

		for (i = 0; i < sat_array->len; i++) {
			g_boxed_free (GYPSY_CLIENT_SATELLITES_CHANGED_TYPE,
				      g_ptr_array_index (sat_array, i));
		}
		g_ptr_array_free (sat_array, TRUE);
	}

	priv->new_sat_count = 0;
}

