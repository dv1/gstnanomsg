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



#ifndef GSTNANOMSGSINK_H____
#define GSTNANOMSGSINK_H____

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>


G_BEGIN_DECLS


typedef struct _GstNanomsgSink GstNanomsgSink;
typedef struct _GstNanomsgSinkClass GstNanomsgSinkClass;


#define GST_TYPE_NANOMSGSINK             (gst_nanomsgsink_get_type())
#define GST_NANOMSGSINK(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_NANOMSGSINK,GstNanomsgSink))
#define GST_NANOMSGSINK_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_NANOMSGSINK,GstNanomsgSinkClass))
#define GST_NANOMSGSINK_CAST(obj)        ((GstNanomsgSink *)(obj))
#define GST_IS_NANOMSGSINK(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_NANOMSGSINK))
#define GST_IS_NANOMSGSINK_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_NANOMSGSINK))


struct _GstNanomsgSink
{
	GstBaseSink parent;

	/* nanomsg socket URI (with the prefix) */
	gchar *uri;
	
	/* nanomsg scalability protocol to use
	 * currently, PIPELINE and PUBSUB are supported (the others are
	 * bidirectional, which makes little sense in GStreamer elements)*/
	int protocol;

	/* Whether or not to only use IPv4 (and no IPv6) addresses */
	gboolean ipv4only;

	/* main_fd is the SP socket descriptor of the main sender socket
	 * ctrl_fds are the pipe sockets used for controlling the receiver loop
	 * ctrl_fds[0] is the read end, ctrl_fds[1] the write end
	 * watch_fd is used to check for the SP socket's write status with poll() */
	int main_fd, ctrl_fds[2], watch_fd;

	/* TRUE if the sink is currently in flushing mode */
	gboolean flushing;

	GMutex mutex;
};


struct _GstNanomsgSinkClass
{
	GstBaseSinkClass parent_class;
};


GType gst_nanomsgsink_protocol_get_type(void);


GType gst_nanomsgsink_get_type(void);


G_END_DECLS


#endif
