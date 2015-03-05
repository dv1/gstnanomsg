gstnanomsg
==========

About
-----

This is a set of [GStreamer 1.0](http://gstreamer.freedesktop.org/) plugins for input/output using the [nanomsg socket library](http://nanomsg.org/).


License
-------

These plugins are licensed under the LGPL v2.1.


Available plugins
-----------------

* `nanomsgsrc` : source element using a nanomsg SP socket
* `nanomsgsink` : sink element using a nanomsg SP socket


Dependencies
------------

You'll need a GStreamer 1.0 installation, and nanomsg version 0.5 beta or newer.


Building and installing
-----------------------

This project uses the [waf meta build system](https://code.google.com/p/waf/). To configure , first set
the following environment variables to whatever is necessary for cross compilation for your platform:

* `CC`
* `CFLAGS`
* `LDFLAGS`
* `PKG_CONFIG_PATH`
* `PKG_CONFIG_SYSROOT_DIR`

Then, run:

    ./waf configure --prefix=PREFIX

(The aforementioned environment variables are only necessary for this configure call.)
PREFIX defines the installation prefix, that is, where the built binaries will be installed.

Once configuration is complete, run:

    ./waf

This builds the plugins.
Finally, to install, run:

    ./waf install


Supported nanomsg scalability protocols
---------------------------------------

Currently, the PUB/SUB and PIPELINE protocols are supported, since these are unidirectional. Bidirectional protocols such as Request/Reply do not have clearly separated sinks and sources, making their use in GStreamer less practical.

The source element can work as either a PUB/SUB subscriber, or a PIPELINE pull end. Likewise, the sink element can function as either a PUB/SUB publisher, or a PIPELINE push end.


Additional notes
----------------

* The source and sink elements accept URIs which start with `nn`. Example: `nntcp://127.0.0.1:54000`. This prefix has been added to make sure there are no collisions with other URI handling elements. Internally, the prefix is skipped before the URI is passed to nanomsg.
* `nanomsgsrc` has a `timeout` property. If this property is nonzero, it specifies the number of milliseconds the source should wait before posting a timeout message to the GStreamer pipeline bus. This is useful for reporting that no data has arrived yet, for example.
* `nanomsgsrc` also has a `subscription-topic` property. This sets the subscription topic of a PUB/SUB subscriber, and is only used if the source's protocol is set to `sub`. Refer to the [nanomsg pub/sub documentation](http://nanomsg.org/v0.5/nn_pubsub.7.html) for details about topics.
