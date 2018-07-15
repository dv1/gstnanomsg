/*
 *   GStreamer source element using the nanomsg library
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

#include "gstnanomsgsrc.h"
#include "gstnanomsg.h"


GST_DEBUG_CATEGORY_STATIC(nanomsgsrc_debug);
#define GST_CAT_DEFAULT nanomsgsrc_debug


enum
{
	PROP_0,
	PROP_URI,
	PROP_TIMEOUT,
	PROP_PROTOCOL,
	PROP_IPV4ONLY,
	PROP_RCVBUFSIZE,
	PROP_RCVMAXSIZE,
	PROP_SUBSCRIPTION_TOPIC,
	PROP_IS_LIVE
};


#define DEFAULT_URI NULL
#define DEFAULT_TIMEOUT 0
#define DEFAULT_PROTOCOL NN_PULL
#define DEFAULT_IPV4ONLY TRUE
#define DEFAULT_RCVBUFSIZE (128 * 1024)
#define DEFAULT_RCVMAXSIZE (1024 * 1024)
#define DEFAULT_SUBSCRIPTION_TOPIC NULL


#define LOCK_SRC_MUTEX(OBJ)    g_mutex_lock(&(((GstNanomsgSrc*)(OBJ))->mutex))
#define UNLOCK_SRC_MUTEX(OBJ)  g_mutex_unlock(&(((GstNanomsgSrc*)(OBJ))->mutex))


static GstStaticPadTemplate static_src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS_ANY
);


static void gst_nanomsgsrc_uri_handler_init(gpointer iface, gpointer iface_data);


G_DEFINE_TYPE_WITH_CODE(
	GstNanomsgSrc, gst_nanomsgsrc, GST_TYPE_PUSH_SRC,
	G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER, gst_nanomsgsrc_uri_handler_init)
)


static void gst_nanomsgsrc_finalize(GObject *object);
static void gst_nanomsgsrc_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_nanomsgsrc_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static GstStateChangeReturn gst_nanomsgsrc_change_state(GstElement *element, GstStateChange transition);

static GstFlowReturn gst_nanomsgsrc_create(GstPushSrc *psrc, GstBuffer **buf);
static gboolean gst_nanomsgsrc_unlock(GstBaseSrc *bsrc);
static gboolean gst_nanomsgsrc_unlock_stop(GstBaseSrc *bsrc);

static gboolean gst_nanomsgsrc_init_sockets(GstNanomsgSrc *nanomsgsrc);
static void gst_nanomsgsrc_shutdown_sockets(GstNanomsgSrc *nanomsgsrc);

static GstURIType gst_nanomsgsrc_uri_get_type(GType type);
static const gchar* const * gst_nanomsgsrc_uri_get_protocols(GType type);
static gchar* gst_nanomsgsrc_uri_get_uri(GstURIHandler *handler);
static gboolean gst_nanomsgsrc_uri_set_uri(GstURIHandler *handler, const gchar *uri, GError **error);

static gboolean gst_nanomsgsrc_update_rcvbufsize(GstNanomsgSrc *nanomsgsrc, int new_size);
static gboolean gst_nanomsgsrc_update_rcvmaxsize(GstNanomsgSrc *nanomsgsrc, int new_size);




GType gst_nanomsgsrc_protocol_get_type(void)
{
	static GType gst_nanomsgsrc_protocol_type = 0;

	if (!gst_nanomsgsrc_protocol_type)
	{
		static GEnumValue protocol_values[] =
		{
			{ NN_PULL, "Push-pull pipeline receiver", "pull" },
			{ NN_SUB, "Publish-subscribe subscriber", "sub" },
			{ 0, NULL, NULL },
		};

		gst_nanomsgsrc_protocol_type = g_enum_register_static(
			"GstNanomsgSrcProtocol",
			protocol_values
		);
	}

	return gst_nanomsgsrc_protocol_type;
}




static void gst_nanomsgsrc_class_init(GstNanomsgSrcClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;
	GstBaseSrcClass *basesrc_class;
	GstPushSrcClass *pushsrc_class;

	GST_DEBUG_CATEGORY_INIT(nanomsgsrc_debug, "nanomsgsrc", 0, "nanomsg-based data source");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);
	basesrc_class = GST_BASE_SRC_CLASS(klass);
	pushsrc_class = GST_PUSH_SRC_CLASS(klass);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_src_template));

	object_class->finalize      = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_finalize);
	object_class->set_property  = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_set_property);
	object_class->get_property  = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_get_property);
	element_class->change_state = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_change_state);
	basesrc_class->unlock       = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_unlock);
	basesrc_class->unlock_stop  = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_unlock_stop);
	pushsrc_class->create       = GST_DEBUG_FUNCPTR(gst_nanomsgsrc_create);

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
	        PROP_TIMEOUT,
	        g_param_spec_uint64(
	                "timeout",
	                "Timeout",
	                "Post a message after timeout nanoseconds (0 = disabled)",
	                0, G_MAXUINT64,
			DEFAULT_TIMEOUT,
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
			gst_nanomsgsrc_protocol_get_type(),
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
	        PROP_RCVBUFSIZE,
	        g_param_spec_int(
	                "rcvbufsize",
	                "Receive buffer size",
	                "Size of the receive buffer, in bytes",
	                1, G_MAXINT,
			DEFAULT_RCVBUFSIZE,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);
	g_object_class_install_property(
	        object_class,
	        PROP_RCVMAXSIZE,
	        g_param_spec_int(
	                "rcvmaxsize",
	                "Max reception size",
	                "Maximum message size that can be received, in bytes (0 = no maximum; received size is limited only by available addressable memory)",
	                0, G_MAXINT,
			DEFAULT_RCVBUFSIZE,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);
	g_object_class_install_property(
	        object_class,
	        PROP_SUBSCRIPTION_TOPIC,
	        g_param_spec_string(
	                "subscription-topic",
	                "Subscription topic",
	                "Which subscription topic to match messages against (null = any message matches) ; only used if the protocol is set to \"sub\"",
	                DEFAULT_SUBSCRIPTION_TOPIC,
	                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
	        )
	);
	g_object_class_install_property(
		object_class,
		PROP_IS_LIVE,
		g_param_spec_boolean(
			"is-live",
			"is-live",
			"Act like a live source",
			FALSE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	gst_element_class_set_static_metadata(
		element_class,
		"nanomsg-based receiver",
		"Source/Network",
		"Receive data via a nanomsg SP socket",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);
}


static void gst_nanomsgsrc_init(GstNanomsgSrc *nanomsgsrc)
{
	nanomsgsrc->uri = DEFAULT_URI;
	nanomsgsrc->timeout = DEFAULT_TIMEOUT;
	nanomsgsrc->protocol = DEFAULT_PROTOCOL;
	nanomsgsrc->ipv4only = DEFAULT_IPV4ONLY;
	nanomsgsrc->rcvbufsize = DEFAULT_RCVBUFSIZE;
	nanomsgsrc->rcvmaxsize = DEFAULT_RCVMAXSIZE;
	nanomsgsrc->subscription_topic = DEFAULT_SUBSCRIPTION_TOPIC;

	nanomsgsrc->main_fd = -1;
	nanomsgsrc->ctrl_fds[0] = -1;
	nanomsgsrc->ctrl_fds[1] = -1;

	nanomsgsrc->flushing = FALSE;
	g_mutex_init(&(nanomsgsrc->mutex));

	gst_base_src_set_format(GST_BASE_SRC(nanomsgsrc), GST_FORMAT_TIME);
	gst_base_src_set_do_timestamp(GST_BASE_SRC(nanomsgsrc), TRUE);
}


static void gst_nanomsgsrc_finalize(GObject *object)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(object);

	if (nanomsgsrc->uri != DEFAULT_URI)
		g_free(nanomsgsrc->uri);
	if (nanomsgsrc->subscription_topic != DEFAULT_SUBSCRIPTION_TOPIC)
		g_free(nanomsgsrc->subscription_topic);
	g_mutex_clear(&(nanomsgsrc->mutex));

	G_OBJECT_CLASS(gst_nanomsgsrc_parent_class)->finalize(object);
}


static void gst_nanomsgsrc_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(object);
	switch (prop_id)
	{
		case PROP_URI:
		{
			gchar const *new_uri;

			LOCK_SRC_MUTEX(nanomsgsrc);

			new_uri = g_value_get_string(value);
			if (new_uri != NULL)
			{
				if (nanomsgsrc->uri != DEFAULT_URI)
					g_free(nanomsgsrc->uri);
				nanomsgsrc->uri = g_strdup(new_uri);
				GST_DEBUG_OBJECT(object, "using new URI: %s", nanomsgsrc->uri);
			}
			else
				GST_DEBUG_OBJECT(object, "ignoring NULL URI string");

			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_TIMEOUT:
		{
			LOCK_SRC_MUTEX(nanomsgsrc);
			nanomsgsrc->timeout = g_value_get_uint64(value);
			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_PROTOCOL:
		{
			LOCK_SRC_MUTEX(nanomsgsrc);
			nanomsgsrc->protocol = g_value_get_enum(value);
			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_IPV4ONLY:
		{
			LOCK_SRC_MUTEX(nanomsgsrc);
			nanomsgsrc->ipv4only = g_value_get_boolean(value);
			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_RCVBUFSIZE:
		{
			LOCK_SRC_MUTEX(nanomsgsrc);
			gst_nanomsgsrc_update_rcvbufsize(nanomsgsrc, g_value_get_int(value));
			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_RCVMAXSIZE:
		{
			LOCK_SRC_MUTEX(nanomsgsrc);
			gst_nanomsgsrc_update_rcvmaxsize(nanomsgsrc, g_value_get_int(value));
			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_SUBSCRIPTION_TOPIC:
		{
			gchar const *new_topic;

			LOCK_SRC_MUTEX(nanomsgsrc);

			if (nanomsgsrc->subscription_topic != DEFAULT_SUBSCRIPTION_TOPIC)
				g_free(nanomsgsrc->subscription_topic);

			new_topic = g_value_get_string(value);
			if (new_topic != NULL)
			{
				nanomsgsrc->subscription_topic = g_strdup(new_topic);
				GST_DEBUG_OBJECT(object, "using new subscription topic: %s", nanomsgsrc->subscription_topic);
			}
			else
			{
				nanomsgsrc->subscription_topic = NULL;
				GST_DEBUG_OBJECT(object, "not using any subscription topic (= all messages are accepted)");
			}

			UNLOCK_SRC_MUTEX(nanomsgsrc);

			break;
		}

		case PROP_IS_LIVE:
			gst_base_src_set_live(GST_BASE_SRC(object), g_value_get_boolean(value));
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_nanomsgsrc_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(object);
	switch (prop_id)
	{
		case PROP_URI:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_string(value, nanomsgsrc->uri);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_TIMEOUT:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_uint64(value, nanomsgsrc->timeout);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_PROTOCOL:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_enum(value, nanomsgsrc->protocol);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_IPV4ONLY:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_boolean(value, nanomsgsrc->ipv4only);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_RCVBUFSIZE:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_int(value, nanomsgsrc->rcvbufsize);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_RCVMAXSIZE:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_int(value, nanomsgsrc->rcvmaxsize);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_SUBSCRIPTION_TOPIC:
			LOCK_SRC_MUTEX(nanomsgsrc);
			g_value_set_string(value, nanomsgsrc->subscription_topic);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;

		case PROP_IS_LIVE:
			g_value_set_boolean(value, gst_base_src_is_live(GST_BASE_SRC(object)));
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static GstStateChangeReturn gst_nanomsgsrc_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn result;
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(element);

	switch (transition)
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
		{
			gboolean ok;
			LOCK_SRC_MUTEX(nanomsgsrc);
			ok = gst_nanomsgsrc_init_sockets(nanomsgsrc);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			if (!ok)
				return GST_STATE_CHANGE_FAILURE;
			break;
		}
		default:
			break;
	}

	if ((result = GST_ELEMENT_CLASS(gst_nanomsgsrc_parent_class)->change_state (element, transition)) == GST_STATE_CHANGE_FAILURE)
		return result;

	switch (transition)
	{
		case GST_STATE_CHANGE_READY_TO_NULL:
			LOCK_SRC_MUTEX(nanomsgsrc);
			gst_nanomsgsrc_shutdown_sockets(nanomsgsrc);
			UNLOCK_SRC_MUTEX(nanomsgsrc);
			break;
		default:
			break;
	}

	return result;
}


static GstFlowReturn gst_nanomsgsrc_create(GstPushSrc *psrc, GstBuffer **buf)
{
	gboolean try_again, flushing;
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC_CAST(psrc);
	struct pollfd fds[2];

	g_assert(nanomsgsrc->main_fd >= 0);

	/* Fetch the current state of the flushing flag
	 * this can be modified in the unlock() function by
	 * another thread, so synchronize the access */
	LOCK_SRC_MUTEX(nanomsgsrc);
	flushing = nanomsgsrc->flushing;
	UNLOCK_SRC_MUTEX(nanomsgsrc);

	if (flushing)
		return GST_FLOW_FLUSHING;

	/* Set up structures for the poll() call below */
	fds[0].fd = nanomsgsrc->ctrl_fds[0];
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	fds[1].fd = nanomsgsrc->watch_fd;
	fds[1].events = POLLIN;
	fds[1].revents = 0;

	do
	{
		int ret;
		try_again = FALSE;

		ret = poll(fds, 2, (nanomsgsrc->timeout == 0) ? -1 : nanomsgsrc->timeout);
		if (ret == -1)
		{
			/* poll() failed - exit loop and report error */
			GST_ERROR_OBJECT(nanomsgsrc, "poll() error: %s", strerror(errno));
			return GST_FLOW_ERROR;
		}
		else if (ret == 0)
		{
			/* Timeout was reached; post message and wait again for data */

			GST_LOG_OBJECT(nanomsgsrc, "timeout reached - trying again");
			try_again = TRUE;

			gst_element_post_message(
				GST_ELEMENT_CAST(nanomsgsrc),
				gst_message_new_element(
					GST_OBJECT_CAST(nanomsgsrc),
					gst_structure_new(
						"GstNanomsgSrcTimeout",
						"timeout", G_TYPE_UINT64, nanomsgsrc->timeout,
						NULL
					)
				)
			);
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
					GST_ERROR_OBJECT(nanomsgsrc, "error while receiving wakeup call over control pipe: %s", strerror(errno));

				LOCK_SRC_MUTEX(nanomsgsrc);
				flushing = nanomsgsrc->flushing;
				UNLOCK_SRC_MUTEX(nanomsgsrc);

				if (flushing)
					return GST_FLOW_FLUSHING;
				else
				{
					/* Set try_again to TRUE in case no flushing is
					 * actually needed and no data arrived yet */
					try_again = TRUE;
				}
			}

			/* NOTE: no "else if". Sometimes, while the unlock() call mentioned
			 * above is finished (instantly followed by the unlock_stop() call
			 * sometimes), data is immediately sent to the SP socket, which means
			 * that there is data to read. */
			if (fds[1].revents & POLLIN)
			{
				GstBuffer *outbuf;
				char *msg = NULL;
				GstFlowReturn flow_ret;
				int msg_size;

				/* Get the message from the SP socket. NN_MSG means the returned
				 * msg pointer refers to a buffer allocated internally by nanomsg
				 * with the appropriate size. This is considered more efficient,
				 * since nanomsg may be using zerocopy mechanisms here (and not
				 * actually be allocating anything). Refer to the nn_recv()
				 * reference for details.
				 * Using nn_recv() with the NN_DONTWAIT flag, as an extra error check.
				 * If nn_send() fails because it cannot write anything, then something
				 * is wrong inside nanomsg, since we already waited with poll() for
				 * writing to be possible. Without the NN_DONTWAIT we wouldn't get an
				 * error, and block forever instead.*/
				if ((msg_size = nn_recv(nanomsgsrc->main_fd, &msg, NN_MSG, NN_DONTWAIT)) < 0)
				{
					GST_ERROR_OBJECT(nanomsgsrc, "error while trying to receive data: %s", strerror(errno));
					return GST_FLOW_ERROR;
				}

				/* Create a GstBuffer with the appropriate size */
				if ((flow_ret = GST_BASE_SRC_CLASS(gst_nanomsgsrc_parent_class)->alloc(GST_BASE_SRC_CAST(psrc), -1, msg_size, &outbuf)) != GST_FLOW_OK)
				{
					GST_DEBUG_OBJECT(nanomsgsrc, "could not allocate new output buffer");
					nn_freemsg(msg);
					return flow_ret;
				}

				/* Copy the message's bytes over to the GstBuffer */
				gst_buffer_fill(outbuf, 0, msg, msg_size);

				/* Signal nanomsg that the message buffer isn't needed anymore */
				nn_freemsg(msg);

				/* Data *was* actually received, so make sure the loop is exited
				 * (try_again may have been set to TRUE after the control pipe
				 * received a wakeup call even though no flushing was required) */
				try_again = FALSE;

				/* Done */
				*buf = outbuf;
			}
		}
	}
	while (G_UNLIKELY(try_again));

	return GST_FLOW_OK;
}


static gboolean gst_nanomsgsrc_unlock(GstBaseSrc *bsrc)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC_CAST(bsrc);
	GST_LOG_OBJECT(bsrc, "flushing");

	LOCK_SRC_MUTEX(nanomsgsrc);
	nanomsgsrc->flushing = TRUE;
	UNLOCK_SRC_MUTEX(nanomsgsrc);

	/* Wake up the poll() function inside the create()
	 * function, since it must now finish immediately
	 * (the actual message is irrelevant) */
	if (write(nanomsgsrc->ctrl_fds[1], "wakeup", 6) == -1)
		GST_ERROR_OBJECT(nanomsgsrc, "error while sending wakeup call over control pipe: %s", strerror(errno));

	return TRUE;
}


static gboolean gst_nanomsgsrc_unlock_stop(GstBaseSrc *bsrc)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC_CAST(bsrc);
	GST_LOG_OBJECT(bsrc, "no longer flushing");

	LOCK_SRC_MUTEX(nanomsgsrc);
	nanomsgsrc->flushing = FALSE;
	UNLOCK_SRC_MUTEX(nanomsgsrc);

	return TRUE;
}


static gboolean gst_nanomsgsrc_init_sockets(GstNanomsgSrc *nanomsgsrc)
{
	/* This function must be called with a mutex lock */

	int rc;
	size_t watch_fd_size = sizeof(nanomsgsrc->watch_fd);
	gchar const *actual_uri;

	/* No URI -> cannot initialize anything */
	if (nanomsgsrc->uri == NULL)
	{
		GST_ELEMENT_ERROR(nanomsgsrc, RESOURCE, NOT_FOUND, ("No URI specified"), (NULL));
		return FALSE;
	}

	/* Check for the URI scheme prefix */
	if (!g_str_has_prefix(nanomsgsrc->uri, PROTOCOL_PREFIX))
	{
		GST_ELEMENT_ERROR(nanomsgsrc, RESOURCE, NOT_FOUND, ("URI starts with the wrong prefix"), ("URI: %s , expected prefix: \"" PROTOCOL_PREFIX "\"", nanomsgsrc->uri));
		return FALSE;
	}
	
	/* Get the actual nanomsg URI, without the prefix */
	actual_uri = PROTOCOLSTR_REMOVE_PREFIX(nanomsgsrc->uri);
	GST_DEBUG_OBJECT(nanomsgsrc, "URI without the prefix: %s", actual_uri);

	/* Cleanup any existing sockets before initializing new ones */
	gst_nanomsgsrc_shutdown_sockets(nanomsgsrc);


	/* Set up the control pipe file descriptors */
	if (G_UNLIKELY(pipe(nanomsgsrc->ctrl_fds) < 0))
	{
		GST_ERROR_OBJECT(nanomsgsrc, "could not create control pipe: %s", strerror(errno));
		goto failure;
	}


	/* The actual SP socket setup */

	/* Create a SP socket, using the currently configured protocol */
	if (G_UNLIKELY((nanomsgsrc->main_fd = nn_socket(AF_SP, nanomsgsrc->protocol)) < 0))
	{
		GST_ERROR_OBJECT(nanomsgsrc, "could not create SP socket: %s", strerror(errno));
		goto failure;
	}

	/* Configure the socket with the given URI */
	rc = 0;
	switch (nanomsgsrc->protocol)
	{
		case NN_PULL:
			rc = nn_bind(nanomsgsrc->main_fd, actual_uri);
			break;

		case NN_SUB:
			rc = (rc < 0) ? rc : nn_setsockopt(nanomsgsrc->main_fd, NN_SUB, NN_SUB_SUBSCRIBE, (nanomsgsrc->subscription_topic != NULL ? nanomsgsrc->subscription_topic : ""), 0);
			rc = (rc < 0) ? rc : nn_connect(nanomsgsrc->main_fd, actual_uri);
			break;

		default:
			GST_ERROR_OBJECT(nanomsgsrc, "invalid protocol value %d", nanomsgsrc->protocol);
			g_assert_not_reached();
	}

	if (G_UNLIKELY(rc < 0))
	{
		GST_ERROR_OBJECT(nanomsgsrc, "could not configure the SP socket: %s", strerror(errno));
		goto failure;
	}

	/* Retrieve the watch FD for use with poll() */
	if (G_UNLIKELY(nn_getsockopt(nanomsgsrc->main_fd, NN_SOL_SOCKET, NN_RCVFD, (char*)&(nanomsgsrc->watch_fd), &watch_fd_size)) < 0)
	{
		GST_ERROR_OBJECT(nanomsgsrc, "could not retrieve watch file descriptor: %s", strerror(errno));
		goto failure;
	}


	/* Misc settings */

	if (G_UNLIKELY(!gst_nanomsgsrc_update_rcvbufsize(nanomsgsrc, nanomsgsrc->rcvbufsize)))
		goto failure;

	if (G_UNLIKELY(!gst_nanomsgsrc_update_rcvmaxsize(nanomsgsrc, nanomsgsrc->rcvmaxsize)))
		goto failure;

	{
		int ipv4only = nanomsgsrc->ipv4only ? 1 : 0;
		if (G_UNLIKELY(nn_setsockopt(nanomsgsrc->main_fd, NN_SOL_SOCKET, NN_IPV4ONLY, (char const*)&(ipv4only), sizeof(ipv4only))) < 0)
		{
			GST_ERROR_OBJECT(nanomsgsrc, "could not configure IPV4ONLY flag: %s", strerror(errno));
			goto failure;
		}
	}


	/* Make sure flushing is disabled in the beginning */
	nanomsgsrc->flushing = FALSE;


	/* All done */
	GST_DEBUG_OBJECT(nanomsgsrc, "initialized SP socket with URI %s", actual_uri);

	return TRUE;


failure:
	gst_nanomsgsrc_shutdown_sockets(nanomsgsrc);
	return FALSE;
}


static void gst_nanomsgsrc_shutdown_sockets(GstNanomsgSrc *nanomsgsrc)
{
	/* This function must be called with a mutex lock */

	if (nanomsgsrc->main_fd >= 0)
	{
		if (nn_close(nanomsgsrc->main_fd) < 0)
			GST_ERROR_OBJECT(nanomsgsrc, "error while closing SP socket: %s", strerror(errno));

		nanomsgsrc->main_fd = -1;
		nanomsgsrc->watch_fd = -1;
	}

	if (nanomsgsrc->ctrl_fds[0] >= 0)
		close(nanomsgsrc->ctrl_fds[0]);
	if (nanomsgsrc->ctrl_fds[1] >= 0)
		close(nanomsgsrc->ctrl_fds[1]);
	nanomsgsrc->ctrl_fds[0] = -1;
	nanomsgsrc->ctrl_fds[1] = -1;

	GST_DEBUG_OBJECT(nanomsgsrc, "shut down SP socket");

}


static void gst_nanomsgsrc_uri_handler_init(gpointer iface, G_GNUC_UNUSED gpointer iface_data)
{
	GstURIHandlerInterface *uri_handler_iface = (GstURIHandlerInterface *)iface;

	uri_handler_iface->get_type = gst_nanomsgsrc_uri_get_type;
	uri_handler_iface->get_protocols = gst_nanomsgsrc_uri_get_protocols;
	uri_handler_iface->get_uri = gst_nanomsgsrc_uri_get_uri;
	uri_handler_iface->set_uri = gst_nanomsgsrc_uri_set_uri;
}


static GstURIType gst_nanomsgsrc_uri_get_type(G_GNUC_UNUSED GType type)
{
	return GST_URI_SRC;
}


static gchar const * const * gst_nanomsgsrc_uri_get_protocols(G_GNUC_UNUSED GType type)
{
	return gstnanomsg_supported_protocols;
}


static gchar* gst_nanomsgsrc_uri_get_uri(GstURIHandler *handler)
{
	gchar *uri;
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(handler);

	LOCK_SRC_MUTEX(nanomsgsrc);
	uri = g_strdup(nanomsgsrc->uri);
	UNLOCK_SRC_MUTEX(nanomsgsrc);

	return uri;
}


static gboolean gst_nanomsgsrc_uri_set_uri(GstURIHandler *handler, const gchar *uri, G_GNUC_UNUSED GError **error)
{
	GstNanomsgSrc *nanomsgsrc = GST_NANOMSGSRC(handler);

	LOCK_SRC_MUTEX(nanomsgsrc);
	nanomsgsrc->uri = g_strdup(uri);
	UNLOCK_SRC_MUTEX(nanomsgsrc);

	return TRUE;
}


static gboolean gst_nanomsgsrc_update_rcvbufsize(GstNanomsgSrc *nanomsgsrc, int new_size)
{
	nanomsgsrc->rcvbufsize = new_size;

	if (nanomsgsrc->main_fd < 0)
		return TRUE;

	if (G_UNLIKELY(nn_setsockopt(nanomsgsrc->main_fd, NN_SOL_SOCKET, NN_RCVBUF, &new_size, sizeof(new_size)) == -1))
	{
		GST_ERROR_OBJECT(nanomsgsrc, "error while setting new receive buffer size: %s", strerror(errno));
		return FALSE;
	}
	else
		return TRUE;
}


static gboolean gst_nanomsgsrc_update_rcvmaxsize(GstNanomsgSrc *nanomsgsrc, int new_size)
{
	nanomsgsrc->rcvmaxsize = new_size;

	if (nanomsgsrc->main_fd < 0)
		return TRUE;

	/* Max size -1 means "no limit". But a max size of 0 makes no sense, so we
	 * interpret 0 as "no limit". For this reason, we have to replace a 0 with
	 * -1 to make sure nanomsg lifts the size limit. */
	if (new_size == 0)
		new_size = -1;

	if (G_UNLIKELY(nn_setsockopt(nanomsgsrc->main_fd, NN_SOL_SOCKET, NN_RCVMAXSIZE, &new_size, sizeof(new_size)) == -1))
	{
		GST_ERROR_OBJECT(nanomsgsrc, "error while setting new max receive size: %s", strerror(errno));
		return FALSE;
	}
	else
		return TRUE;
}
