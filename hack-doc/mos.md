# mos code review.

* The major mos initialization code lies in mtcp_create_context in core.c.

* After the initialization, each thread runs MTCPDPDKRunThread to do the actual work.

* The socket_type in mptcp_api.h

# Several goals for a formally verified TCP/IP stack.

* Memory safety. Correct with respect to specification.

* Run on both server and embeded devices, to support IoT.
