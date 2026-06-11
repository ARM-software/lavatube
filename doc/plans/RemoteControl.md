# Plan for lava-cli remote control

Motivation: Make debugging easier with a new command-line tool to break and inspect
running trace replays.

Inspired by [Renderdoc's CLI tool](https://github.com/BANANASJIM/rdc-cli).

## Functionality

The basic functionality is a new option to `lava-replay` that makes it start providing
a remote controlled service:

```
lava-replay --service <my trace file>.vk
```

When called like this, it launches a background thread that listens on a TCP port, then
waits for further instructions on this port.

These instructions are sent from a new tool `lava-cli`.

Already implemented instructions:
* `lava-cli status` - show our basic status
* `lava-cli continue` - continues the replay until the end
* `lava-cli stop` - stops the replay
* `lava-cli step [packets X|calls X]` - step the given number of packets or API calls ahead
* `lava-cli goto X|NAME` - continue replay until API call number X or the next API call named NAME
* `lava-cli info threads`
* `lava-cli params|parameters` - print command or packet input parameters as JSON
* `lava-cli show <object type> <index>` - print given globally tracked object and its metadata as JSON
	- commandbuffers : would be good to be able to print their command contents, but no way to introspect this at the moment, we can only show what we store for execute_commands()

More instructions to implement - in prioritized order:
* `lava-cli thread N` - updates the stored current thread index
* `lava-cli continue` - also receive debug info from all threads, and if replay fails then print the last debug information received
* `lava-cli step frames X` - step the given number of frames ahead in the current thread, then pause again
- `lava-cli goto frame X` - replay until we get to the given frame
* `lava-cli info <topic>` - show input parameters and important state
	- 'objects' - show all non-zero object types, with pending, created, bound (if applicable) and destroyed columns
	- 'queues'
	- 'swapchains' - show image index numbers of real and fake swapchains and their status
* `lava-cli list <object type>` - list all objects of given type tracked globally and their status
* `lava-cli save buffer|image|tensor <index> <filename>` - write exact contents of object given by index to the given filename (if bound)
* `lava-cli convert buffer|image|tensor <index> <filename.png>` - transform to linear format and write contents of image data given by index to the given filename (if bound)
* `lava-cli set debug <level>` - change global debug level
* `lava-cli set blackhole <true|false>` - change blackhole setting
* `lava-cli instrument [detailed]` - on `vkBeginCommandBuffer` to instrument the commandbuffer, returns the index of the cmdbuffer
* `lava-cli show instrumentation <cmdbuf index>` - attempt to fetch all `VK_ARM_shader_instrumentation` data from the given cmdbuffer by index

## Notes

We deliberately pause _after_ command execution so that we can inspect the results from
the command. This hides some state from us, however. Any stored inputs that get overwritten
by the executed command with new data will not be visible. We could make sure both data sets
are kept and have a switch to choose whcih one to show, though.

## shader instrumentation

Make use of [VK_ARM_shader_instrumentation](https://docs.vulkan.org/features/latest/features/proposals/VK_ARM_shader_instrumentation.html),
there is some documentation [here](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ARM_shader_instrumentation.html).
Can test with GPU model.

Inject instrumentation probe into the commandbuffer under construction, requires currently constructing a commandbuffer.
Once the commandbuffer has been submitted, we will also inject a wait for execution to finish then print the results.

We don't need to create one in advance, just create one just as we need it, unless we can reuse one from a pool. Biggest
problem is that when we are the command we want to instrument, it is too late, we need to do it on the command _before_,
hence we just instrument every draw or dispatch in the entire commandbuffer.

By default entire commandbuffer is instrumented in one measurement, but if 'detailed' is added then each dispatch or draw
is instrumented separately; references to instrumentations are stored in the commandbuffer meta object.

## Binaries

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

## Manipulation

It could sometimes be useful to manipulate (ie change) existing data structures for
testing purposes. However, since we don't pause before calling commands, we can't
modify what goes into command execution, only what is stored as a result.

Ideas:
* Modify SPIRV in shader module (basically recreating it)
* Instrumenting a commandbuffer (adding write-out after a command while commandbuffer
  recording is started)

## Multi-threading

Lavatube is multithreaded and traces _can_ be heavily multi-threaded. If we pause one thread,
the other threads will continue until they hit their next synchronization point. We will want
to make sure we let all threads hit their next synchronization point before we return control
to `lava-cli` (possibly means that we need an atomic state machine status in each running
thread to say whether we are running or waiting, unless there is some other way to check this,
eg see if we are at the next synchronization point or not).

One issue we will have is our use of spinlocks for most replay thread synchronizations. This
means even though the process is in 'pause' state, it will consume quite a bit of CPU. (Also
the moment we step beyond a waiting point, threads waiting for it will race ahead.)

## State machine

We should have a very simple state machine: We can be in states `paused`, `running` or `done`.
When in `running` state we only accept the `stop` command. For most commands, we need to be
in the `paused` state.

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

## Output format

## Tabular data

Allow choice of display of tabular data in either markdown or CSV (or TSV, tab separated).
We have a new class in `src/datatable.h` for abstracting away the choice.

We should also allow saving directly to file. JSON/JSONL is often recommended for accuracy
for nested or mixed data, while CSV/TSV for strictly regular data.

There is no perfect format for neither accuracy nor token efficiency, it all depends on the
data.

Output type may therefore depend on `--human-readable` vs `--machine-readable` or similar
option. May also want `--always-ndjson`.

## Nested / structured data

We currently use NDJSON / json lines for nested data. We should consider a more human-readable
format as well.

### Image data

Plan is to support both binary blobs and conversion to PNG. [This](doc/plans/CompressedAssetsFile.md)
might be related.

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

## Additional questions to resolve (from Gemini review)

### Thread Pausing Behavior

* **Synchronization Points:** The plan mentions: *"If we pause one thread, the other threads
 will continue until they hit their next synchronization point."*

* **Risk:** Vulkan apps can have worker threads that just process compute shaders or build
 command buffers for a long time without hitting a heavy Vulkan synchronization primitive. If
 you wait for them to hit a sync point before giving control to `lava-cli`, the tool might hang
 indefinitely.

* **Recommendation:** When a pause is requested (or a breakpoint/step completes), you should
 probably flip a global `atomic<bool> is_paused` flag. **All** threads should check this flag
 right before they dispatch their next Vulkan command. This ensures all threads freeze at their
 current command boundaries almost immediately, rather than waiting for an API-level
 synchronization point.

### Stepping Semantics Across Threads

* **The "Step" Command:** When you `lava-cli step call 1` on Thread A, what should Thread B do?

* **Recommendation:** Usually, in debuggers, if you step a specific thread, the other threads
 remain frozen to prevent the state from changing under your feet. You should explicitly define
 this behavior in the plan. If `lava-cli step` only advances the active thread while keeping
 others blocked on a condition variable, you need to ensure this doesn't cause deadlocks if
 Thread A's next command is waiting on a fence signaled by Thread B. Providing a `step-all`
 vs `step-thread` distinction might be necessary eventually.
