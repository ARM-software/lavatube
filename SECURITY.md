Reporting security issues
-------------------------

Until people actually start using this software in production and
sharing traces, there is no reason to be coy about any security
issues you might find. Just open a regular issue or merge request
in our github project.

Security concerns
-----------------

There are two usage patterns for this software: Capture and replay
(replay including here post-processing or inspect).

This software has no privileges that the captured app does not
already have during capture, so there is nothing to be gained from
securing it.

Replay (parsing the trace file in any way with this software) can,
however, introduce an untrusted input into a trusted environment if
the trace file comes from outside the trusted environment (eg a
customer or an outside bug report), and this carries with it the
risk of a security breach.

Security design
---------------

API tracing exposes a very large attack surface to tools that can be
exploited by a clever attacker by modifying a trace file by hand and
sending it to an unsuspecting victim. Instead of attempting to harden
this entire surface entrypoint by entrypoint, lavatube is
experimenting with a sandbox to reduce what can be gained by such an
attack.

This sandbox is composed of four levels:

* Zero - sandbox disabled.
* One - ability of getting root privileges removed.
* Two - network and file execution privileges removed.
* Three - file system write privileges removed except to current
  working directory and directories underneath it.

The first level is entered on program startup, the second after
parsing command line arguments, and the third just before parsing
the trace file. This allows us to gradually drop privileges as
as they are no longer needed.

However, Vulkan layers are also affected by these capability changes,
so validation layers, screenshot layers, and performance tools might
all become dysfunctional.
