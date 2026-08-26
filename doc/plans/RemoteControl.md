# Plan for lava-cli remote control

Motivation: Make debugging easier with a new command-line tool to break and inspect
running trace replays.

Terminology:
* `lava-cli` is the _controller_ (whoever calls it is the _agent_)
* `lava-replay` is the _replay service_ (its underlying system is the _replay host system_)

Inspired by [Renderdoc's CLI tool](https://github.com/BANANASJIM/rdc-cli).

## Functionality

The basic functionality is a new `--service` option to `lava-replay` that makes it
start providing a remote controlled service. When called like this, it launches a
background thread that listens on a TCP port, then waits for further instructions
on this port. These instructions are sent from a new tool `lava-cli`.

More instructions to implement:
* `lava-cli validation update` - similar to `log update`, just get the latest validation warnings; if no validation layer enabled, just return 'ERROR'
* `lava-cli validation tail [REGEX=*] [limit=10] [since=LINE] [update=on|off]` - similar to `log tail` above
* `lava-cli step THREAD frames X` - step the given number of frames ahead in THREAD, then pause again
* `lava-cli goto THREAD frame X` - replay THREAD until it gets to the given frame
* `lava-cli repeat` - repeat the current call (without incrementing the packet count); not all calls are safe to repeat - must use common sense
* `lava-cli info <topic>` - show input parameters and important state
	- 'objects' - show table of all non-zero-sized object types, with pending, created, bound (if applicable) and destroyed columns
	- 'swapchains' - show image index numbers of real and fake swapchains and their status
	- 'devices' - list the available GPU physical devices
	- 'device <device index>' - information about the given GPU device through `vkGetPhysicalDeviceProperties2` to get `VkPhysicalDeviceProperties` and `VkPhysicalDeviceDriverProperties` - dump key info from each as ndjson, mark the current one
	- 'system' - information about the current system (non-GPU), report back Linux (kernel) version, Android version (or Windows) version, as ndjson
	- 'extensions <device index>' - print supported and enabled extensions
* `lava-cli list <object type> [filter=all|created|bound|destroyed] [limit=20]` - list all objects of given type tracked globally and their status
* `lava-cli save buffer|image|tensor <index> <filename>` - write exact contents of object given by index to the given filename (if bound; possibly using staging)
* `lava-cli convert image <index> <filename.png>` - transform to linear format and write contents of image data given by index to the given filename (if bound; possibly using staging; as PNG)
* `lava-cli writeout buffer|image <index> <filename>` - inject a scheduled write-out at the current position in the current commandbuffer, it will be written after queue submit;
  push a callback on the queue submit so we can queuewaitidle -> write -> clear writeout queue
* `lava-cli split-cmdbuf-by-renderpass` - only when on `vkBeginCommandBuffer` to split commandbuffers by renderpasses
* `lava-cli split-cmdbuf-by-shader` - only when on `vkBeginCommandBuffer` to split commandbuffers by shader calls
* `lava-cli add-markers THREAD` - only when THREAD is on `vkBeginCommandBuffer` to sprinkle marker calls into the generated commandbuffer; returns the target marker session and command buffer
* `lava-cli read-markers` - read out the last value of our markers, useful if we encountered a device lost error during execution of a marked up commandbuffer
* `lava-cli backtrace` - generate a backtrace from the current position, could invoke gdb, need to figure out what to do for Android
	- on same host, llm model could just fire up gdb, which is far more powerful, so of limited usefulness
	- could we annotate the backtrace with extra info to increase usefulness?
	- use case: could trigger a backtrace if the process is hanging (triggering it from pause position does not seem terribly useful)
* `lava-cli inject <packet> <thread> device-wait <device index>` - inject a future wait for the given device before the given packet boundary; this only makes sense if we `continue` or `goto` past this point at full speed
	- we should keep all inject operations in a sorted queue, one per thread, so we can quickly check the current packet against the top of the queue (we can have multiple injected packets pending on one packet)
* `lava-cli inject <packet> <thread> fence-wait <device index> <fence index>` - similar to the above, inject a future wait for the given fence before the given packet boundary
* `lava-cli inject <packet> <thread> queue-wait <queue index>` - similar to the above, inject a future wait for the given queue before the given packet boundary
* `lava-cli inject <packet> <thread> host-wait <packet> <thread>` - similar to the above, inject a future barrier waiting for the given other thread and packet before the given packet boundary

## Notes

We deliberately pause _after_ command execution so that we can inspect the results from
the command. This hides some state from us, however. Any stored inputs that get overwritten
by the executed command with new data will not be visible. We could make sure both data sets
are kept and have a switch to choose which one to show, though.

Guarantees that we should give:
* Upon resumption of a command, all threads are back in pause mode.
* We run vkDeviceWaitIdle to ensure all issued GPU work has completed.
* Either we return DEVICE_LOST or device is intact (we check return value of vkDeviceWaitIdle before returning).

## Crashes

When we get an unexpected return value from the replayed API, a plain `lava-replay`
run voluntarily aborts, and this makes it unreachable from `lava-cli` - which is
especially tricky if the replay happens on a remote device. In replay service mode
(`lava-replay --service`) we now go into an inspectable error state instead of
aborting.

This is now mostly implemented.

Still to do:
* An explicit 'ignore' command for selected non-fatal errors (not useful for device
  lost, but maybe for some others).
* Several special generated cases detect bad results with `assert` instead of
  `check_retval()` (eg some swapchain and queue functions); those should route
  through the same service-mode handling.
* The environment mismatch aborts scattered through the replay code (memory types,
  swapchain formats, queue families, ...) should also become `ABORTED` service
  states rather than process aborts, while true invariant/trace corruption keeps
  the hard abort.
* The user of `lava-cli` needs to be clearly informed that we paused at a different
  place than what was requested, and if the active thread was changed this also needs to
  be communicated (the `result=` suffix and thread number are a first step).

## Fetching binaries

We can get binary data from a number of sources:
* `lava-cli params` may reference a binary blob of data; we should give it an attachment
  index number and push it onto an attachment list with pointer to the data in our memory
  pool, allowing `lava-cli save attachment <inded> <filename>` to save it to file.
* Global metadata may reference it, eg `laval-cli show VkImage index 1`, here we may store
  the referenced data with `lava-cli save <type> <index> <filename>`.
* It may be ephemeral data only used during the running of a commandbuffer. Here we need
  to recreate and instrument the commandbuffer to store the data. We should only do this
  when waiting on a queue submit, and on return from the queue submit we should wait for
  queue to finish then write out the result. But we wait _after_ the queue submit, which
  makes this hard. Either we special case queue submits, or we awkwardly ask users to
  instrument on a command before. Less awkward if we could have a goto that put as at the
  command _before_ the target. Another option is to require the user to go to the command
  creation where we could add output target copy command into the commandbuffer without
  any re-creation.

Whenever we specify a filename, we could instead specify 'network', at which point we
should respond with a single word of text then in binary: uint64_t size + data blob;
since we would be streaming this from memory bandwidth constrained devices, we need to
work hard to avoid unnecessary memory copies. Also, read-back from non-cached VRAM is
extremely slow, forcing the source memory into cached host memory may make it a lot
faster. Single word 'SEND' or 'FAIL'.

## Manipulation

It could sometimes be useful to manipulate (ie change) existing data structures for
testing purposes. However, since we don't pause before calling commands, we can't
modify what goes into command execution, only what is stored as a result.

Ideas:
* Modify SPIRV in shader module (basically recreating it)
* Instrumenting a commandbuffer (adding write-out after a command while commandbuffer
  recording is started)

## Multi-threading

Lavatube is multithreaded and traces can be heavily multi-threaded. If we pause one thread,
the other threads will continue until they hit their next synchronization point. We want
to make sure we let all threads hit their next synchronization point before we return control
to `lava-cli`.

One issue we will have is our use of spinlocks for most replay thread synchronizations. This
means even though the process is in 'pause' state, it will consume quite a bit of CPU, and
the moment we step beyond a waiting point, threads waiting for it will race ahead.

## Security

We must allocate all our TCP bindings before we enter sandbox level higher than 1. Sandbox
should still work with service mode, but must be done in the right order so we create
resources needing rights before we enter a sandbox level that prohibits them.

# Open questions for later

## Capture

On Android in particular capturing might be challenging to setup and debug. A remote control
link might make it easier.

## lava-tool

There might be some value in running `lava-tool` also in service mode with remote control,
especially if debugging the spirv-simulator.

## Android

Should service mode be the default? Then replay is always remote controlled from linux. Makes
easy debug always easy.

## Logs

We already have the ability to stream debug output to file. We can use our control channel
to fetch the current contents of the file - possibly with an offset if we already fetched parts
of it.

## Output formats

Allow choice of display of tabular data in either markdown or CSV (or TSV, tab separated).
We have a new class in `src/datatable.h` for abstracting away the choice.

We currently use NDJSON / json lines for nested data. We should consider a more human-readable
format as well.

For image data, the plan is to support both binary blobs and conversion to PNG.
[This](doc/plans/CompressedAssetsFile.md) might be related.

## Implementation

Raw berkeley sockets seems like the best match. We need a separate control thread in the
tool that we communicate with. We can connect to Android using `adb forward`.

For capture, by default we attempt to wait for a connection for some time (eg 200 ms) before
spawning a thread to keep waiting. This means our client must hammer the capture platform
with requests roughly once every 100 ms to have a good chance of connecting from the very
start. We can change the wait time with env var `LAVATUBE_WAIT_TIME`, set to zero for no
waiting, -1 to wait forever, or any other positive value to wait this many milliseconds.

For replay, we only spawn a control thread when explicitly requested to do so, and in this
case we wait for a connection before proceeding. We spawn our control thread only after we
have received a connection and been told to proceed.

If there is a disconnect during capture, we keep going. There is no good other option here.
If there is a disconnect during replay, we have more options, but keep going is the least
invasive, so we should go with this to begin with at least.

## lava-tool service mode (written by codex)

There is value in allowing `lava-tool` to run in service mode as well, especially for
debugging simulator and post-processing runs. This should reuse the same `lava-cli`
protocol where possible, but the service implementation should not remain tied to
`lava-replay`'s file-scope `replayer` object. The command listener and common helper
functions should be moved into a shared service module that can operate on a currently
active `lava_reader`.

Unlike `lava-replay`, `lava-tool` can run multiple passes. Service mode should pause at
the start of each pass before worker threads are started, so the user can inspect which
pass is about to run and choose when to continue. The service should update its active
`lava_reader` pointer for each pass and expose the pass in status output, for example
analysis/simulation pass 0 and output-rewrite pass 1.

Needed code changes:
* Add `--service`, `-P/--port`, and `-H/--host` handling to `lava-tool`.
* Extract the reusable parts of `src/replay.cpp`'s service listener into shared code.
* Make the shared service target a `lava_reader*` supplied by the current tool pass.
* Add service lifecycle handling around each `lava-tool` pass: start/listen before the
  first pass, set the current reader before pausing, wake worker threads on `continue`,
  and cleanly stop/join the listener on `stop` or normal completion.
* Add `check_cli()` handling to `lava-tool`'s replay loop after `switchboard_packet()`,
  mirroring `lava-replay`, and update `cli_packet` as packets are processed.
* Keep generated and hardcoded parameter serialization shared through the existing
  `cli_params_*` helpers.
* Reset or clearly publish per-pass state so commands do not accidentally inspect a
  finalized previous pass.

Initial command support should include `status`, `continue`, `stop`, `step`, `goto`,
`parameters`, `show`, `info objects`, `info threads`, `info thread`, `info frame`, and
`set debug`. `set blackhole` should not be supported for `lava-tool`, since blackhole
mode only makes sense for actual replay execution and not for tool analysis or trace
rewriting. Runtime-state commands that depend on live Vulkan replay state, such as
`info memory`, `info suballocator`, and pipeline executable statistics, should either be
omitted from `lava-tool` service mode or return a clear unsupported response until there
is a concrete tool use case for them.

## Always-available replay diagnostics (summary by codex)

Normal `lava-replay` runs should expose the remote-control diagnostic endpoint as well,
so that `lava-cli` can attach after an intermittent hang without requiring the hang to be
reproduced with `--service`. This should be an observer mode rather than literally issuing
the current `continue` command: replay should start immediately, retain its normal exit
behavior, and preserve the `check_cli()` fast path by leaving `cli_thread == -1` until a
client requests control.

The listener should bind to localhost only by default. Normal runs cannot all use the same
fixed port because parallel replays and tests would collide, so they should use an ephemeral
port or a PID-addressable Unix socket. The selected endpoint must be printed or registered
against the replay PID so that `lava-cli` can discover it. Failure to allocate the optional
diagnostic endpoint should not make an otherwise valid normal replay fail.

Setting `cli_service` in observer mode allows replay threads to publish barrier, handle,
fence, queue-idle, and device-idle waits for `status`, `info threads`, and
`diagnose deadlock`. Expensive optional facilities such as pipeline executable statistics,
memory-budget reporting, and shader instrumentation should remain disabled until explicitly
requested, rather than changing normal device setup or replay performance.

To move from observation to interactive debugging, add an explicit `pause` or `interrupt`
command. It should atomically select a replay thread and bring all replay threads to stable
pause or synchronization points before returning. Existing stepping and inspection commands
can then operate normally. This requires separating "the replay is running" from "a blocking
CLI command is active", which are currently both represented by `cli_running`.

The listener adds one polling thread and a local endpoint, but the replay hot path should
otherwise retain its existing one-atomic fast exit from `check_cli()`. On normal completion,
the listener must stop automatically and `lava-replay` must exit as it does today. Explicit
`--service` mode may continue to start paused and remain available after replay reaches
`DONE` until the client asks it to stop.

## Concurrent query caveats

Only explicitly classified observer commands run concurrently. Commands such as
`parameters THREAD` remain exclusive because parameter publication currently uses one
global request flag and response buffer, even though fetching parameters is conceptually
a query.

Replay and system log updates also remain exclusive because streams currently share one
service-owned cursor. Supporting independent concurrent log readers requires moving the
cursor into the request, or otherwise storing it per client, so one reader cannot consume
another reader's updates.
