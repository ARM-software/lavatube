# Test suite

Main document is [this doc](Agent.md).

We want to measure the effectiveness of `lava-agent` settings along
two axes: Time and quality. We should develop a test suite of questions
about our current trace files, with correct answers stored. Then
record the time it takes to get responses and an evaluation (score 1 to 10)
of their responses. We should send one warmup question first to make sure
the LLM model is fully loaded first.

Things to test:
- Changes to the system prompt
- Time and number of rounds allowed
- Models: Gemma4 vs Ornith vs Terra vs Luna
	- Also comparing different quants of Gemma
	- Want to see if an on-device mobile-sized model could work

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
