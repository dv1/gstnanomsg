/*
 *   GStreamer sink element using the nanomsg library
 *   Copyright (C) 2015 Carlos Rafael Giani
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * SECTION: element-nanomsgsink
 *
 * nanomsgsink is a sink that sends data via a nanomsg SP socket.
 * It can be combined with rtp payload encoders to implement RTP streaming.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 audiotestsrc ! nanomsgsink uri=nnipc:///tmp/pipeline.ipc
 * ]| Send data to a nanomsg sink using an IPC connection
 * |[
 * gst-launch-1.0 nanomsgsrc uri=nnipc:///tmp/pipeline.ipc ! fakesink
 * ]| Receive data from the pipeline above
 *
 * |[
 * gst-launch-1.0 audiotestsrc ! rtpgstpay ! nanomsgsink uri=nntcp:///192.168.0.1:54001 protocol=pub
 * ]| Send audio stream packets enveloped in the GST RTP payloader and transmit it over TCP, using the publish/subscribe protocol (the publisher side)
 * |[
 * gst-launch-1.0 nanomsgsrc do-timestamp=false uri=nnipc:///tmp/192.168.0.1:54001 protocol=sub ! "application/x-rtp, encoding-name=X-GST" ! rtpgstdepay ! autoaudiosink
 * ]| Receive data from the pipeline above using the publish/subscribe protocol (the subscriber side), depayload it (disabling the incoming timestamps since these are uninteresting here), and play
 *
 * Note in the example above that the subscriber pipeline can be started more than once, on different threads, processes, or hosts (see the nanomsg documentation for details about publish/subscribe)
 *
 * </refsect2>
 */



#include <errno.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <nanomsg/pubsub.h>

#include "gstnanomsgsink.h"
#include "gstnanomsg.h"


GST_DEBUG_CATEGORY_STATIC(nanomsgsink_debug);
#define GST_CAT_DEFAULT nanomsgsink_debug


enum
{
	PROP_0,
	PROP_URI,
	PROP_PROTOCOL,
	PROP_IPV4ONLY,
	PROP_SNDBUFSIZE
};


#define DEFAULT_URI NULL
#define DEFAULT_PROTOCOL NN_PUSH
#define DEFAULT_IPV4ONLY TRUE
#define DEFAULT_SNDBUFSIZE (128 * 1024)


#define LOCK_SINK_MUTEX(OBJ)    g_mutex_lock(&(((GstNanomsgSink*)(OBJ))->mutex))
#define UNLOCK_SINK_MUTEX(OBJ)  g_mutex_unlock(&(((GstNanomsgSink*)(OBJ))->mutex))


static GstStaticPadTemplate static_sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS_ANY
);


static void gst_nanomsgsink_uri_handler_init(gpointer iface, gpointer iface_data);


G_DEFINE_TYPE_WITH_CODE(
	GstNanomsgSink, gst_nanomsgsink, GST_TYPE_BASE_SINK,
	G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER, gst_nanomsgsink_uri_handler_init)
)


static void gst_nanomsgsink_finalize(GObject *object);
static void gst_nanomsgsink_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_nanomsgsink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static gboolean gst_nanomsgsink_start(GstBaseSink *bsink);
static gboolean gst_nanomsgsink_stop(GstBaseSink *bsink);
static gboolean gst_nanomsgsink_unlock(GstBaseSink *bsink);
static gboolean gst_nanomsgsink_unlock_stop(GstBaseSink *bsink);
static GstFlowReturn gst_nanomsgsink_render(GstBaseSink *bsink, GstBuffer *buffer);

static gboolean gst_nanomsgsink_init_sockets(GstNanomsgSink *nanomsgsink);
static void gst_nanomsgsink_shutdown_sockets(GstNanomsgSink *nanomsgsink);

static GstURIType gst_nanomsgsink_uri_get_type(GType type);
static const gchar* const * gst_nanomsgsink_uri_get_protocols(GType type);
static gchar* gst_nanomsgsink_uri_get_uri(GstURIHandler *handler);
static gboolean gst_nanomsgsink_uri_set_uri(GstURIHandler *handler, const gchar *uri, GError **error);

static gboolean gst_nanomsgsink_update_sndbufsize(GstNanomsgSink *nanomsgsink, int new_size);




GType gst_nanomsgsink_protocol_get_type(void)
{
	static GType gst_nanomsgsink_protocol_type = 0;

	if (!gst_nanomsgsink_protocol_type)
	{
		static GEnumValue protocol_values[] =
		{
			{ NN_PUSH, "Push-pull pipeline sender", "push" },
			{ NN_PUB, "Publish-subscribe publisher", "pub" },
			{ 0, NULL, NULL },
		};

		gst_nanomsgsink_protocol_type = g_enum_register_static(
			"GstNanomsgSinkProtocol",
			protocol_values
		);
	}

	return gst_nanomsgsink_protocol_type;
}




static void gst_nanomsgsink_class_init(GstNanomsgSinkClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstBaseSinkClass *basesink_class;

	GST_DEBUG_CATEGORY_INIT(nanomsgsink_debug, "nanomsgsink", 0, "nanomsg-based data sink");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	basesink_class = GST_BASE_SINK_CLASS(klass);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_sink_template));

	object_class->finalize      = GST_DEBUG_FUNCPTR(gst_nanomsgsink_finalize);
	object_class->set_property  = GST_DEBUG_FUNCPTR(gst_nanomsgsink_set_property);
	object_class->get_property  = GST_DEBUG_FUNCPTR(gst_nanomsgsink_get_property);
	basesink_class->start        = GST_DEBUG_FUNCPTR(gst_nanomsgsink_start);
	basesink_class->stop         = GST_DEBUG_FUNCPTR(gst_nanomsgsink_stop);
	basesink_class->unlock       = GST_DEBUG_FUNCPTR(gst_nanomsgsink_unlock);
	basesink_class->unlock_stop  = GST_DEBUG_FUNCPTR(gst_nanomsgsink_unlock_stop);
	basesink_class->render       = GST_DEBUG_FUNCPTR(gst_nanomsgsink_render);

	g_object_class_install_property(
	        object_class,
	        PROP_URI,
	        g_param_spec_string(
	                "uri",
	                "URI",
	                "Where to bind the SP socket to",
	                DEFAULT_URI,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);
	g_object_class_install_property(
		object_class,
		PROP_PROTOCOL,
		g_param_spec_enum(
			"protocol",
			"Protocol",
			"Scalability protocol to use",
			gst_nanomsgsink_protocol_get_type(),
			DEFAULT_PROTOCOL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
	g_object_class_install_property(
	        object_class,
	        PROP_IPV4ONLY,
	        g_param_spec_boolean(
	                "ipv4only",
	                "IPv4Only",
	                "Whether or not to use only IPv4 addresses (as opposed to both IPv4 and IPv6 ones)",
	                DEFAULT_IPV4ONLY,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);
	g_object_class_install_property(
	        object_class,
	        PROP_SNDBUFSIZE,
	        g_param_spec_int(
	                "sndbufsize",
	                "Send buffer size",
	                "Size of the send buffer, in bytes",
	                1, G_MAXINT,
			DEFAULT_SNDBUFSIZE,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);

	gst_element_class_set_static_metadata(
		element_class,
		"nanomsg-based sender",
		"Sink/Network",
		"Receive data via a nanomsg SP socket",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);
}


static void gst_nanomsgsink_init(GstNanomsgSink *nanomsgsink)
{
	nanomsgsink->uri = NULL;
	nanomsgsink->protocol = DEFAULT_PROTOCOL;
	nanomsgsink->ipv4only = DEFAULT_IPV4ONLY;
	nanomsgsink->sndbufsize = DEFAULT_SNDBUFSIZE;

	nanomsgsink->main_fd = -1;
	nanomsgsink->watch_fd = -1;
	nanomsgsink->ctrl_fds[0] = -1;
	nanomsgsink->ctrl_fds[1] = -1;

	nanomsgsink->flushing = FALSE;
	g_mutex_init(&(nanomsgsink->mutex));
}


static void gst_nanomsgsink_finalize(GObject *object)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK(object);

	if (nanomsgsink->uri != DEFAULT_URI)
		g_free(nanomsgsink->uri);
	g_mutex_clear(&(nanomsgsink->mutex));

	G_OBJECT_CLASS(gst_nanomsgsink_parent_class)->finalize(object);
}


static void gst_nanomsgsink_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK(object);
	switch (prop_id)
	{
		case PROP_URI:
		{
			gchar const *new_uri;

			LOCK_SINK_MUTEX(nanomsgsink);

			new_uri = g_value_get_string(value);
			if (new_uri != NULL)
			{
				g_free(nanomsgsink->uri);
				nanomsgsink->uri = g_strdup(new_uri);
				GST_DEBUG_OBJECT(object, "using new URI: %s", nanomsgsink->uri);
			}
			else
				GST_DEBUG_OBJECT(object, "ignoring NULL URI string");

			UNLOCK_SINK_MUTEX(nanomsgsink);

			break;
		};

		case PROP_PROTOCOL:
		{
			LOCK_SINK_MUTEX(nanomsgsink);
			nanomsgsink->protocol = g_value_get_enum(value);
			UNLOCK_SINK_MUTEX(nanomsgsink);

			break;
		}

		case PROP_IPV4ONLY:
		{
			LOCK_SINK_MUTEX(nanomsgsink);
			nanomsgsink->ipv4only = g_value_get_boolean(value);
			UNLOCK_SINK_MUTEX(nanomsgsink);

			break;
		}

		case PROP_SNDBUFSIZE:
		{
			LOCK_SINK_MUTEX(nanomsgsink);
			gst_nanomsgsink_update_sndbufsize(nanomsgsink, g_value_get_int(value));
			UNLOCK_SINK_MUTEX(nanomsgsink);

			break;
		}

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_nanomsgsink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK(object);
	switch (prop_id)
	{
		case PROP_URI:
			LOCK_SINK_MUTEX(nanomsgsink);
			g_value_set_string(value, nanomsgsink->uri);
			UNLOCK_SINK_MUTEX(nanomsgsink);
			break;

		case PROP_PROTOCOL:
			LOCK_SINK_MUTEX(nanomsgsink);
			g_value_set_enum(value, nanomsgsink->protocol);
			UNLOCK_SINK_MUTEX(nanomsgsink);
			break;

		case PROP_IPV4ONLY:
			LOCK_SINK_MUTEX(nanomsgsink);
			g_value_set_boolean(value, nanomsgsink->ipv4only);
			UNLOCK_SINK_MUTEX(nanomsgsink);
			break;

		case PROP_SNDBUFSIZE:
			LOCK_SINK_MUTEX(nanomsgsink);
			g_value_set_int(value, nanomsgsink->sndbufsize);
			UNLOCK_SINK_MUTEX(nanomsgsink);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static gboolean gst_nanomsgsink_start(GstBaseSink *bsink)
{
	gboolean ok;
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK_CAST(bsink);

	LOCK_SINK_MUTEX(nanomsgsink);
	ok = gst_nanomsgsink_init_sockets(GST_NANOMSGSINK(bsink));
	UNLOCK_SINK_MUTEX(nanomsgsink);

	return ok;
}


static gboolean gst_nanomsgsink_stop(GstBaseSink *bsink)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK_CAST(bsink);

	LOCK_SINK_MUTEX(nanomsgsink);
	gst_nanomsgsink_shutdown_sockets(GST_NANOMSGSINK(bsink));
	UNLOCK_SINK_MUTEX(nanomsgsink);

	return TRUE;
}


static gboolean gst_nanomsgsink_unlock(GstBaseSink *bsink)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK_CAST(bsink);
	GST_LOG_OBJECT(bsink, "flushing");

	LOCK_SINK_MUTEX(nanomsgsink);
	nanomsgsink->flushing = TRUE;
	UNLOCK_SINK_MUTEX(nanomsgsink);

	/* Wake up the poll() function inside the create()
	 * function, since it must now finish immediately
	 * (the actual message is irrelevant) */
	if (write(nanomsgsink->ctrl_fds[1], "wakeup", 6) == -1)
		GST_ERROR_OBJECT(nanomsgsink, "error while sending wakeup call over control pipe: %s", strerror(errno));

	return TRUE;
}


static gboolean gst_nanomsgsink_unlock_stop(GstBaseSink *bsink)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK_CAST(bsink);
	GST_LOG_OBJECT(bsink, "no longer flushing");

	LOCK_SINK_MUTEX(nanomsgsink);
	nanomsgsink->flushing = FALSE;
	UNLOCK_SINK_MUTEX(nanomsgsink);

	return TRUE;
}


static GstFlowReturn gst_nanomsgsink_render(GstBaseSink *bsink, GstBuffer *buffer)
{
	gboolean try_again, flushing;
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK_CAST(bsink);
	struct pollfd fds[2];

	g_assert(nanomsgsink->main_fd >= 0);

	/* Fetch the current state of the flushing flag
	 * this can be modified in the unlock() function by
	 * another thread, so synchronize the access */
	LOCK_SINK_MUTEX(nanomsgsink);
	flushing = nanomsgsink->flushing;
	UNLOCK_SINK_MUTEX(nanomsgsink);

	if (flushing)
		return GST_FLOW_FLUSHING;

	/* Set up structures for the poll() call below */
	fds[0].fd = nanomsgsink->ctrl_fds[0];
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	/* NOTE: the watch FD associated with the main_fd SP socket is watched
	 * with the POLLIN event flag, *not* POLLOUT, even though main_fd is
	 * an output socket
	 * This is because nanomsg sends notifications to watch_fd, similar to
	 * how the ctrl_fds pipe works */
	fds[1].fd = nanomsgsink->watch_fd;
	fds[1].events = POLLIN;
	fds[1].revents = 0;

	/* Unlike the nanomsg source, the sink uses no timeouts. Still, a loop is used,
	 * to catch cases where the wakeup call sent over the control pipeline woke up
	 * poll() unnecessarily */
	do
	{
		int ret;
		try_again = FALSE;

		ret = poll(fds, 2, -1);
		if (ret == -1)
		{
			/* poll() failed - exit loop and report error */
			GST_ERROR_OBJECT(nanomsgsink, "poll() error: %s", strerror(errno));
			return GST_FLOW_ERROR;
		}
		else
		{
			/* At least one FD got data to read */

			if (fds[0].revents & POLLIN)
			{
				/* Control pipe read end received a wakeup call
				 * Sometimes, unlock_stop() is called so quickly after unlock()
				 * that this loop isn't reached before the unlock_stop() call,
				 * which means that sometimes this wakeup call should actually
				 * be ignored, because the flushing period is over by this point.
				 * For this reason, when a wakeup call is received, the code
				 * checks if flushing is still actually required, and only if
				 * this is the case, the function exits with GST_FLOW_FLUSHING */

				char dummy_buf[1024];
				if (read(fds[0].fd, dummy_buf, sizeof(dummy_buf)) == -1)
					GST_ERROR_OBJECT(nanomsgsink, "error while receiving wakeup call over control pipe: %s", strerror(errno));

				LOCK_SINK_MUTEX(nanomsgsink);
				flushing = nanomsgsink->flushing;
				UNLOCK_SINK_MUTEX(nanomsgsink);

				if (flushing)
					return GST_FLOW_FLUSHING;
				else
				{
					/* Set try_again to TRUE in case no flushing is
					 * actually needed and no data can be sent yet */
					try_again = TRUE;
				}
			}

			/* NOTE: no "else if". Sometimes, while the unlock() call mentioned
			 * above is finished (instantly followed by the unlock_stop() call
			 * sometimes), the SP socket could become writable (for example,
			 * because a receiver was connected to it).
			 * Checking for POLLIN even though the code will write to the SP
			 * socket. Check the pollfd fds setup above for details. */
			if (fds[1].revents & POLLIN)
			{
				GstMapInfo map_info;
				int msg_size;

				/* Send the message to the SP socket. Do so with the NN_DONTWAIT
				 * flag, as an extra error check. If nn_send() fails because it
				 * cannot write anything, then something is wrong inside nanomsg,
				 * since we already waited with poll() for writing to be possible.
				 * Without the NN_DONTWAIT we wouldn't get an error, and block
				 * forever instead. */
				gst_buffer_map(buffer, &map_info, GST_MAP_READ);
				msg_size = nn_send(nanomsgsink->main_fd, map_info.data, map_info.size, NN_DONTWAIT);
				gst_buffer_unmap(buffer, &map_info);

				if (msg_size < 0)
				{
					GST_ERROR_OBJECT(nanomsgsink, "error while trying to send data: %s", strerror(errno));
					return GST_FLOW_ERROR;
				}
				else
					GST_LOG_OBJECT(nanomsgsink, "sent %d bytes", msg_size);

				/* Data *was* actually sent, so make sure the loop is exited
				 * (try_again may have been set to TRUE after the control pipe
				 * received a wakeup call even though no flushing was required) */
				try_again = FALSE;
			}
		}
	}
	while (G_UNLIKELY(try_again));

	return GST_FLOW_OK;
}


static gboolean gst_nanomsgsink_init_sockets(GstNanomsgSink *nanomsgsink)
{
	/* This function must be called with a mutex lock */

	int rc;
	size_t watch_fd_size = sizeof(nanomsgsink->watch_fd);
	gchar const *actual_uri;

	/* No URI -> cannot initialize anything */
	if (nanomsgsink->uri == NULL)
	{
		GST_ELEMENT_ERROR(nanomsgsink, RESOURCE, NOT_FOUND, ("No URI specified"), (NULL));
		return FALSE;
	}

	/* Check for the URI scheme prefix */
	if (!g_str_has_prefix(nanomsgsink->uri, PROTOCOL_PREFIX))
	{
		GST_ELEMENT_ERROR(nanomsgsink, RESOURCE, NOT_FOUND, ("URI starts with the wrong prefix"), ("URI: %s , expected prefix: \"" PROTOCOL_PREFIX "\"", nanomsgsink->uri));
		return FALSE;
	}
	
	/* Get the actual nanomsg URI, without the prefix */
	actual_uri = PROTOCOLSTR_REMOVE_PREFIX(nanomsgsink->uri);
	GST_DEBUG_OBJECT(nanomsgsink, "URI without the prefix: %s", actual_uri);

	/* Cleanup any existing sockets before initializing new ones */
	gst_nanomsgsink_shutdown_sockets(nanomsgsink);


	/* Set up the control pipe file descriptors */
	if (G_UNLIKELY(pipe(nanomsgsink->ctrl_fds) < 0))
	{
		GST_ERROR_OBJECT(nanomsgsink, "could not create control pipe: %s", strerror(errno));
		goto failure;
	}


	/* The actual SP socket setup */

	/* Create a SP socket, using the currently configured protocol */
	if (G_UNLIKELY((nanomsgsink->main_fd = nn_socket(AF_SP, nanomsgsink->protocol))) < 0)
	{
		GST_ERROR_OBJECT(nanomsgsink, "could not create SP socket: %s", strerror(errno));
		goto failure;
	}

	/* Configure the socket with the given URI */
	rc = 0;
	switch (nanomsgsink->protocol)
	{
		case NN_PUSH:
			rc = nn_connect(nanomsgsink->main_fd, actual_uri);
			break;

		case NN_PUB:
			rc = nn_bind(nanomsgsink->main_fd, actual_uri);
			break;

		default:
			GST_ERROR_OBJECT(nanomsgsink, "invalid protocol value %d", nanomsgsink->protocol);
			g_assert_not_reached();
	}

	if (G_UNLIKELY(rc < 0))
	{
		GST_ERROR_OBJECT(nanomsgsink, "could not configure the SP socket: %s", strerror(errno));
		goto failure;
	}

	/* Retrieve the watch FD for use with poll() */
	if (G_UNLIKELY(nn_getsockopt(nanomsgsink->main_fd, NN_SOL_SOCKET, NN_SNDFD, (char*)&(nanomsgsink->watch_fd), &watch_fd_size) < 0))
	{
		GST_ERROR_OBJECT(nanomsgsink, "could not retrieve watch file descriptor: %s", strerror(errno));
		goto failure;
	}


	/* Misc settings */

	if (G_UNLIKELY(!gst_nanomsgsink_update_sndbufsize(nanomsgsink, nanomsgsink->sndbufsize)))
		goto failure;

	{
		int ipv4only = nanomsgsink->ipv4only ? 1 : 0;
		if (G_UNLIKELY(nn_setsockopt(nanomsgsink->main_fd, NN_SOL_SOCKET, NN_IPV4ONLY, (char const*)&(ipv4only), sizeof(ipv4only))) < 0)
		{
			GST_ERROR_OBJECT(nanomsgsink, "could not configure IPV4ONLY flag: %s", strerror(errno));
			goto failure;
		}
	}


	/* Make sure flushing is disabled in the beginning */
	nanomsgsink->flushing = FALSE;


	/* All done */
	GST_DEBUG_OBJECT(nanomsgsink, "initialized SP socket with URI %s", actual_uri);

	return TRUE;


failure:
	gst_nanomsgsink_shutdown_sockets(nanomsgsink);
	return FALSE;
}


static void gst_nanomsgsink_shutdown_sockets(GstNanomsgSink *nanomsgsink)
{
	/* This function must be called with a mutex lock */

	if (nanomsgsink->main_fd >= 0)
	{
		if (nn_close(nanomsgsink->main_fd) < 0)
			GST_ERROR_OBJECT(nanomsgsink, "error while closing SP socket: %s", strerror(errno));

		nanomsgsink->main_fd = -1;
		nanomsgsink->watch_fd = -1;
	}

	if (nanomsgsink->ctrl_fds[0] >= 0)
		close(nanomsgsink->ctrl_fds[0]);
	if (nanomsgsink->ctrl_fds[1] >= 0)
		close(nanomsgsink->ctrl_fds[1]);
	nanomsgsink->ctrl_fds[0] = -1;
	nanomsgsink->ctrl_fds[1] = -1;

	GST_DEBUG_OBJECT(nanomsgsink, "shut down SP socket");
}


static void gst_nanomsgsink_uri_handler_init(gpointer iface, G_GNUC_UNUSED gpointer iface_data)
{
	GstURIHandlerInterface *uri_handler_iface = (GstURIHandlerInterface *)iface;

	uri_handler_iface->get_type = gst_nanomsgsink_uri_get_type;
	uri_handler_iface->get_protocols = gst_nanomsgsink_uri_get_protocols;
	uri_handler_iface->get_uri = gst_nanomsgsink_uri_get_uri;
	uri_handler_iface->set_uri = gst_nanomsgsink_uri_set_uri;
}


static GstURIType gst_nanomsgsink_uri_get_type(G_GNUC_UNUSED GType type)
{
	return GST_URI_SINK;
}


static gchar const * const * gst_nanomsgsink_uri_get_protocols(G_GNUC_UNUSED GType type)
{
	return gstnanomsg_supported_protocols;
}


static gchar* gst_nanomsgsink_uri_get_uri(GstURIHandler *handler)
{
	gchar *uri;
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK(handler);

	LOCK_SINK_MUTEX(nanomsgsink);
	uri = g_strdup(nanomsgsink->uri);
	UNLOCK_SINK_MUTEX(nanomsgsink);

	return uri;
}


static gboolean gst_nanomsgsink_uri_set_uri(GstURIHandler *handler, const gchar *uri, G_GNUC_UNUSED GError **error)
{
	GstNanomsgSink *nanomsgsink = GST_NANOMSGSINK(handler);

	LOCK_SINK_MUTEX(nanomsgsink);
	nanomsgsink->uri = g_strdup(uri);
	UNLOCK_SINK_MUTEX(nanomsgsink);

	return TRUE;
}


static gboolean gst_nanomsgsink_update_sndbufsize(GstNanomsgSink *nanomsgsink, int new_size)
{
	nanomsgsink->sndbufsize = new_size;

	if (nanomsgsink->main_fd < 0)
		return TRUE;

	if (G_UNLIKELY(nn_setsockopt(nanomsgsink->main_fd, NN_SOL_SOCKET, NN_SNDBUF, &new_size, sizeof(new_size)) == -1))
	{
		GST_ERROR_OBJECT(nanomsgsink, "error while setting new send buffer size: %s", strerror(errno));
		return FALSE;
	}
	else
		return TRUE;
}
