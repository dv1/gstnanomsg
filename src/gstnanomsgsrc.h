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



#ifndef GSTNANOMSGSRC_H____
#define GSTNANOMSGSRC_H____

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>


G_BEGIN_DECLS


typedef struct _GstNanomsgSrc GstNanomsgSrc;
typedef struct _GstNanomsgSrcClass GstNanomsgSrcClass;


#define GST_TYPE_NANOMSGSRC             (gst_nanomsgsrc_get_type())
#define GST_NANOMSGSRC(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_NANOMSGSRC,GstNanomsgSrc))
#define GST_NANOMSGSRC_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_NANOMSGSRC,GstNanomsgSrcClass))
#define GST_NANOMSGSRC_CAST(obj)        ((GstNanomsgSrc *)(obj))
#define GST_IS_NANOMSGSRC(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_NANOMSGSRC))
#define GST_IS_NANOMSGSRC_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_NANOMSGSRC))


struct _GstNanomsgSrc
{
	GstPushSrc parent;

	/* nanomsg socket URI (with the prefix) */
	gchar *uri;

	/* timeout interval for poll(); if this interval runs out, a message is
	 * posted informing about the timeout, and poll() is run again */
	int timeout;

	/* nanomsg scalability protocol to use
	 * currently, PIPELINE and PUBSUB are supported (the others are
	 * bidirectional, which makes little sense in GStreamer elements)*/
	int protocol;

	/* Whether or not to only use IPv4 (and no IPv6) addresses */
	gboolean ipv4only;

	/* the subscription topic to use with the PUBSUB protocol; unused with others */
	gchar *subscription_topic;

	/* main_fd is the SP socket descriptor of the main receiver socket
	 * ctrl_fds are the pipe sockets used for controlling the receiver loop
	 * ctrl_fds[0] is the read end, ctrl_fds[1] the write end
	 * watch_fd is used to check for the SP socket's read status with poll() */
	int main_fd, ctrl_fds[2], watch_fd;

	/* TRUE if the source is currently in flushing mode */
	gboolean flushing;

	GMutex mutex;
};


struct _GstNanomsgSrcClass
{
	GstPushSrcClass parent_class;
};


GType gst_nanomsgsrc_protocol_get_type(void);


GType gst_nanomsgsrc_get_type(void);


G_END_DECLS


#endif
