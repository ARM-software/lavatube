# lava-agent

New agentic experiment based on experiences from `lava-cli` and `lava-tui`.

Where `lava-cli` became a useful but primitive diagnostic tool, `lava-tui`
became unused because it was not as powerful as existing agent harnesses.
The useful parts of `lava-tui` - its close tool integration, prompt control
and local model support - can perhaps be utilized better in a different
type of tool.

`lava-agent` is meant as a subagent that operates with a limited number of
steps on a single query and reports back its findings in a structured format.
It is meant to be invoked by an external agent harness, called the controller
in this document, which will typically use a cloud model. `lava-agent` uses a
local model and runs on the same device as the replay service.

It requires:
* Local access to the trace file
* Access to an explicitly selected, already-running `lava-replay` service
  instance that it can query. The caller owns the service lifecycle and replay
  position.
* Access to a local AI model supporting an OpenAI compatible interface (like
  `lava-tui`)
* A fixed system prompt that tells it what it can and must (not) do
* A provided user prompt that tells it what to investigate
* Check with `lava-cli info trace` that the local trace path and the running
  replay refer to the same filesystem object. A lightweight device and inode
  comparison is sufficient; this is an accidental-mismatch check, not a
  security boundary, and does not require hashing the trace.
* Ability to look directly into a trace file similarly to `lava-print-fast`

It will give you:
* Structured JSON output

It must not:
* Modify a running `lava-replay` service instance. The list of tool commands
  must not include options to do so.
* Launch, load a trace into, restart, stop, or advance a `lava-replay` service
  instance.
* Require any state or history. Any context it needs should be provided by the
  controller.

We do not want:
- Persistent history
- Replay control
- Arbitrary commands
- Trace modification
- Human chat UI
- Model routing

## Interface

The simplest request would be something like:

`lava-agent --service localhost:11901 example.api ask "Is thread 3 blocked on work from another replay thread?"`

This would connect to the explicitly selected replay service and read
`example.api` locally to answer the posed question. It would not launch the
service, load the trace into it, or move its replay position.

The prompt implies that there is a running replay service that we can look at,
and we should check the trace status then investigate the trace file to find if
the suggested state is plausible. Then report whatever evidence turned up.

There could be the following options:

```
lava-agent 0.0.2-debug command line options
lava-agent --service HOST:PORT [options] <trace file> ask <prompt>
-h/--help              This help
-v/--verbose           Verbose output (if debug file enabled)
-d/--debug level       Set debug level [0,1,2,3] (if debug file enabled)
-df/--debugfile FILE   Output debug output to the given file
--service HOST:PORT     Required replay service endpoint
--timeout SECONDS       Maximum wall-clock duration (default 300)
--max-output-bytes N    Maximum result size (default 32768)
```

It should return structured evidence along the lines of:

```json
  {
    "schema_version": 1,
    "status": "answered",
    "conclusion": "...",
    "confidence": 0.8,
    "evidence": [
      {
          "tool_id": <number>,
          "tool_name": <string>,
          "arguments": { ... },
          "results": { ... },
          "inference": <string>
      }
    ],
    "unresolved": ["..."],
    "usage": {
      "rounds": 3,
      "calls": 6
    }
  }
```

Version 1 shall emit exactly one compact JSON object followed by a newline on
standard output. No diagnostics or progress messages may be written there.
Diagnostics go to standard error and complete transcripts go only to the debug
file. Once argument parsing has started an invocation, runtime and model
failures shall also produce a valid result envelope. `--help` is the only mode
that writes non-JSON output to standard output.

Possible statuses are `answered`, `inconclusive`, `budget_exhausted`, `unstable`
and `error`. If budget is exhausted, the caller can increase the budget and try
again or try something else. The process shall exit successfully for `answered`,
`inconclusive`, `budget_exhausted`, and `unstable`. It shall exit nonzero only
for `error` or an invalid invocation; the controller shall use the JSON status
to distinguish normal investigation outcomes.

At the start of each `lava-agent` invocation, it shall require the replay service
to be paused and record its position. Immediately before returning its final
result, it shall check that the service is still paused at the same position. If
either check fails, it shall return `unstable` while preserving any evidence it
collected. It does not need to lock the service or poll repeatedly between these
two checks.

## Tools

`lava-agent` shall have its own compile-time allowlist of typed tools. It may
share lower-level implementations with `lava-tui`, but shall not reuse the TUI
tool registry or expose arbitrary `lava-cli` command strings. In particular,
the allowlist must not acquire replay-control tools merely because they are
available to the TUI.

Immutable facts, such as packets, objects, frames, and captured metadata, shall
come from the local trace file. Live facts, such as replay thread positions and
waits, deadlock diagnosis, and current memory state, shall come from the replay
service. Model-visible tool names shall use `trace_` and `replay_` prefixes so
that the provenance is apparent in evidence.

Service tools may call Vulkan and may use the service's exclusive-command mutex
as long as they do not progress any replay thread or change replay
configuration. Diagnostic results do not need to have been cached already.
The current `parameters THREAD` command shall not be exposed because selecting
a different thread can progress that thread; it can be reconsidered after that
behavior is fixed.

The initial replay-service tool allowlist is:

* `replay_get_status`
* `replay_list_threads`
* `replay_diagnose_deadlock`
* `replay_diagnose_device`
* `replay_get_memory`
* `replay_get_suballocator`
* `replay_get_object_state`
* `replay_get_instrumentation`
* `replay_get_as_build`

The initial local-trace tool allowlist is:

* `trace_get_metadata`
* `trace_list_threads`
* `trace_get_thread`
* `trace_get_frame`
* `trace_list_objects`
* `trace_get_object`
* `trace_get_packets`, for one packet or a bounded packet range
* `trace_find_calls`, for an exact command name with optional thread and packet
  range filters and a bounded number of results

The local tools shall not expose arbitrary files from the trace container.
Regex search is deferred.

Wall-clock duration and the size of the result returned to the cloud controller
are the user-facing budgets. The runtime shall also enforce generous internal
sanity limits on model rounds, total tool calls, individual and accumulated
tool-result sizes, packet ranges, and search hits. These internal limits protect
reliability and local-model context rather than optimize local-model token cost.
If the runtime cannot finish within its duration or internal budgets, it shall
return `budget_exhausted` with any useful partial evidence.

The default wall-clock limit is 300 seconds and the default structured-result
limit is 32768 bytes. The controller may override either limit.

The `lava-agent` runtime shall assign an immutable ID to each tool call and
record its validated arguments and actual result. The model may cite those IDs
and provide an inference, but it may not author the call or result records. The
runtime shall reject unknown evidence IDs and construct the final `evidence`
array from its own records. The model requests each next round of calls and does
the inference. The final `evidence` array shall contain only cited calls. When a
debug file is enabled, the runtime shall record the complete tool-call
transcript there, including exploratory calls that were not cited. The
`usage.calls` value still counts all calls.

# Cleanup

We probably should clean up and refector the TUI code a bit so that we can reuse
as much of it as possible.

# Future possibilities

* Have human readable markdown output as an option instead of JSON
  `-m/--markdown          Output in markdown format instead of JSON`
* Regex search capability for calls and logs
* Structured input as cmd line option
  `-i/--input FILE        Take prompt as structured JSON input instead`
* Allow not having a replay service running, with cmd line option
  `-l/--local-only        Do not connect to a replay service`
* A `go` mode (as opposed to `ask`) that uses a different system prompt and tool
  options to allow the agent to modify the running replay service, such as
  "go to the start of the first frame that is not a loading frame".

# Example controller questions (writen by codex)

These examples are intended as evaluation prompts for the controller. The
answers are approximate: packet indices and counts below describe the current
checked-in or locally generated traces and may change when a trace is
regenerated. A useful answer should cite the trace or replay tool calls that
support it, and should not claim a live replay fact from trace data alone.

## How is the mip chain generated?

Trace: `traces/sample_texture_mipmap_generation.api`

Question: "How does this trace generate the mip levels for its texture? Give
the source and destination extent of every step, and say whether the work is
recorded on more than one trace thread."

Approximate answer: It records nine `vkCmdBlitImage` calls on trace thread 0,
all using image 8 and linear filtering. They successively blit mip 0 through
mip 8 into mip 1 through mip 9, with extents 512x512 -> 256x256 -> 128x128 ->
64x64 -> 32x32 -> 16x16 -> 8x8 -> 4x4 -> 2x2 -> 1x1. The surrounding 23
`vkCmdPipelineBarrier` calls perform the required layout transitions. The
trace has only thread 0, so none of this work was recorded concurrently.

Useful tools: `trace_list_threads`, `trace_find_calls` for
`vkCmdBlitImage` and `vkCmdPipelineBarrier`, and bounded
`trace_get_packets` around the nine blits.

## Where does the apparent multithreading happen?

Trace: `traces/demo_multithreading.api`

Question: "Is this application submitting Vulkan work from many threads, or
only recording command buffers from many threads? Summarize the evidence."

Approximate answer: The trace contains 15 threads (0 through 14). Threads 1
through 14 repeatedly record draw command buffers; for example, thread 2 has
280 `vkCmdDrawIndexed` calls together with matching begin/end, pipeline,
vertex/index-buffer, viewport, scissor, and push-constant calls. Submission and
presentation are centralized on thread 0: all 14 `vkQueueSubmit` calls and all
10 `vkQueuePresentKHR` calls occur there. Thus the trace is multithreaded in
command-buffer recording, not queue submission.

Useful tools: `trace_list_threads`, `trace_get_thread`, and
`trace_find_calls` for `vkCmdDrawIndexed`, `vkQueueSubmit`, and
`vkQueuePresentKHR`, filtered by thread.

## What ray-tracing workload is dispatched?

Trace: `traces/demo_raytracingbasic.api`

Question: "Describe the acceleration structures and ray dispatches in this
trace. How many are there, what types are built, and what are the dispatch
dimensions?"

Approximate answer: The trace creates and builds two acceleration structures:
index 0 is type 1 (bottom level), backed by buffer 3 with size 384 bytes, and
index 1 is type 0 (top level), backed by buffer 6 with size 512 bytes. Each is
built once with `vkCmdBuildAccelerationStructuresKHR`. One ray-tracing
pipeline is created. Four recorded `vkCmdTraceRaysKHR` calls dispatch
1280x720x1 rays; their ray-generation, miss, and hit shader-binding-table
regions each have size and stride 32, while the callable region is empty.

Useful tools: `trace_find_calls` for the acceleration-structure and ray-tracing
commands, bounded `trace_get_packets` around their results, and
`trace_get_object` for the referenced buffers and acceleration structures.

## Why might native presentation fail while offscreen replay works?

Trace: `traces/lunarg_vkcube_wayland.api`

Question: "The paused replay has a presentation/device problem, but this trace
can replay offscreen. Is there a captured swapchain choice that plausibly
explains the difference? Separate captured facts from live replay evidence."

Approximate answer: The captured facts show two `vkCreateSwapchainKHR` calls,
at packets 102 and 183. Both request a 500x500, three-image swapchain with
format 97 (`VK_FORMAT_R16G16B16A16_SFLOAT`) and color space 0
(`VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`); the second replaces swapchain 0. This is
a plausible surface-compatibility problem because offscreen replay does not
need the native presentation surface to expose that exact pair. Whether it is
the current failure must come from `replay_diagnose_device` (and current object
state), not from the trace alone. On a service reporting that the pair is
unsupported, the conclusion can be `answered`; without that live evidence it
should remain `inconclusive` or explicitly qualified.

Useful tools: `trace_find_calls` for `vkCreateSwapchainKHR`, bounded
`trace_get_packets`, `replay_diagnose_device`, and
`replay_get_object_state` for the swapchain.

## What orders the three submissions in the timeline-semaphore test?

Trace: `build/vulkan_memory_tracking_race_timeline.api`, generated from the
vendored tracetooltests build.

Question: "Reconstruct the dependency chain between the three queue
submissions. Which semaphore is binary, which is timeline, and which timeline
value connects the submissions?"

Approximate answer: Semaphore 0 is created without a semaphore-type extension,
so it is binary. Semaphore 1 is created with `semaphoreType` 1 and initial
value 0, so it is a timeline semaphore. The first submission, packet 324,
signals binary semaphore 0. The second, packet 326, waits on semaphore 0 and
signals timeline semaphore 1 at value 1. The third, packet 350, waits for value
1 on semaphore 1 and carries fence 0. This forms a strict first -> second ->
third dependency chain. All three submissions use queue 0.

Useful tools: `trace_find_calls` for `vkCreateSemaphore` and `vkQueueSubmit`,
plus `trace_get_packets` for packets 54, 55, 324, 326, and 350.

## Is a blocked replay thread waiting on another replay thread?

Trace: `build/vulkan_memory_tracking_race_timeline.api`, with its replay
service paused at the suspected block.

Question: "Is the blocked replay thread waiting for work that another replay
thread must submit or complete? Identify the captured dependency if possible,
but do not infer the current wait solely from packet order."

Approximate answer: For this particular trace, all captured packets are on
trace thread 0 and all three submissions use queue 0. The captured semaphore
chain is binary semaphore 0 followed by timeline semaphore 1 value 1, as
described above; it does not require a second captured trace thread. A correct
live answer must correlate `replay_list_threads` and
`replay_diagnose_deadlock` with the current packet and semaphore/object state.
If the service shows only this chain and no replay-injected work owned by
another thread, the answer is "no". If the service cannot identify the wait or
its owner, the result should be `inconclusive`, not a guess based on the trace.

Useful tools: `replay_get_status`, `replay_list_threads`,
`replay_diagnose_deadlock`, `replay_get_object_state`, and the trace calls used
by the preceding question.
