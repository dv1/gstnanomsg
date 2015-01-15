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



#ifndef GSTNANOMSG_H____
#define GSTNANOMSG_H____

#include <gst/gst.h>


G_BEGIN_DECLS


#define PROTOCOL_PREFIX "nmsg"
#define PROTOCOL_PREFIX_LENGTH (sizeof(PROTOCOL_PREFIX) - 1)
#define PROTOCOLSTR_ADD_PREFIX(NAME) (PROTOCOL_PREFIX NAME)
#define PROTOCOLSTR_REMOVE_PREFIX(NAME) (&((NAME)[PROTOCOL_PREFIX_LENGTH]))


extern gchar const *gstnanomsg_supported_protocols[];
extern guint const gstnanomsg_num_supported_protocols;


G_END_DECLS


#endif
