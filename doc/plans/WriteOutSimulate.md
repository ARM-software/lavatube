# Write-out with simulation

Where `build/lava-tool -S <input trace> <output trace>` injects `VK_ARM_trace_helpers`
markings into the trace file where we found (using the spriv-simulator and other
methods) that there is memory in need of marking (device addresses, shader group
handles, descriptors in memory, etc.).

We can create `external/tracetooltests` traces without the trace helpers markings
using the `--no-trace-helpers` option, run the tool and verify that with `-S` mode.

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
