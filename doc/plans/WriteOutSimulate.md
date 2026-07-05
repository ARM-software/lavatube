# Write-out with simulation

Where `build/lava-tool -S <input trace> <output trace>` injects `VK_ARM_trace_helpers`
markings into the trace file where we found (using the spriv-simulator and other
methods) that there is memory in need of marking (device addresses, shader group
handles, descriptors in memory, etc.).

We can create `external/tracetooltests` traces without the trace helpers markings
using the `--no-trace-helpers` option, run the tool and verify that with `-S` mode
without an output file we find the missing markings. Then we can verify that with
`-S` and an output file that we get the missing markings, and running with `-S` on
the output (without another output file) makes it stop complaining about missing
markers.

## The -V option

The `-V` option to `lava-tool` is meant to run the same process as `-S` but without
actually writing out a new trace file, just verifying that the existing markers and
the new findings are identical - and abort with an error decribing the difference
if not.

## Non-simulator sources of markings

Currently only aware of `vkCmdTraceRaysIndirectKHR` and `vkCmdTraceRaysIndirect2KHR`.
These two read shader binding table addresses from a generic buffer object, and in
this way reveal to us what is located at the given offsets.

It is already being used in `vulkan_raytracing_3` and `vulkan_raytracing_4`, and
I created a very simple test case `vulkan_raytracing_indirect_noop` as a very simple
test case of the basic workflow.

## Plan

* Add code to track memory marking coming from `vkCmdTraceRaysIndirectKHR` and
  `vkCmdTraceRaysIndirect2KHR`, similar to how spirv-simulator is a source for
  these.
* Add code to verify memory markings coming from `vkCmdTraceRaysIndirect2KHR`
  (verify with `lava-tool -V vulkan_raytracing_indirect_noop.api`)
* We have a trace `vulkan_raytracing_indirect_noop_raw` without memory markings.
  Run `lava-tool -S vulkan_raytracing_indirect_noop_raw.api tmp.api` and verify
  with `packtool diff --assert-markings vulkan_raytracing_indirect.api tmp.api`,
  comparing our manual markings with the newly inserted ones.
