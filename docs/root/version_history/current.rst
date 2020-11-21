1.16.1 (November 20, 2020)
==========================

Incompatible Behavior Changes
-----------------------------
*Changes that are expected to cause an incompatibility if applicable; deployment changes are likely required*

Minor Behavior Changes
----------------------
*Changes that may cause incompatibilities for some users, but should not for most*

Bug Fixes
---------
*Changes expected to improve the state of the world and are unlikely to have negative effects*
* examples: examples use v3 configs.
* listener: fix crash when disabling or re-enabling listeners due to overload while processing LDS updates.
* proxy_proto: fixed a bug where the wrong downstream address got sent to upstream connections.
* tls: fix read resumption after triggering buffer high-watermark and all remaining request/response bytes are stored in the SSL connection's internal buffers.
* udp: fixed issue in which receiving truncated UDP datagrams would cause Envoy to crash.

Removed Config or Runtime
-------------------------
*Normally occurs at the end of the* :ref:`deprecation period <deprecated>`

New Features
------------

Deprecated
----------
