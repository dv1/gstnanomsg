/*
 *   Common code for nanomsg-based GStreamer elements
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



#include <config.h>
#include "gstnanomsg.h"
#include "gstnanomsgsrc.h"
#include "gstnanomsgsink.h"


/* to GStreamer, a protocol schema always starts with the
 * prefix defined by PROTOCOL_PREFIX
 * example: nmsgtcp://127.0.0.1:56000
 * (this avoids conflicts with other URI handlers)
 * the prefix is removed before it is passed to nanomsg
 * the protocol list always ends with NULL, to fulfill the requirements
 * of the URI handler get_protocols() function */
gchar const *gstnanomsg_supported_protocols[] =
{
	PROTOCOLSTR_ADD_PREFIX("tcp"),
	PROTOCOLSTR_ADD_PREFIX("ipc"),
	PROTOCOLSTR_ADD_PREFIX("inproc"),
	NULL
};


/* - 1 , because of the trailing NULL pointer */
guint const gstnanomsg_num_supported_protocols = (sizeof(gstnanomsg_supported_protocols) / sizeof(gchar const *) - 1);


static gboolean plugin_init(GstPlugin *plugin)
{
	gboolean ret = TRUE;
	ret = ret && gst_element_register(plugin, "nanomsgsrc", GST_RANK_PRIMARY + 1, gst_nanomsgsrc_get_type());
	ret = ret && gst_element_register(plugin, "nanomsgsink", GST_RANK_PRIMARY + 1, gst_nanomsgsink_get_type());
	return ret;
}



GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	nanomsg,
	"data transfer elements based on the nanomsg library",
	plugin_init,
	VERSION,
	"LGPL",
	GST_PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)
