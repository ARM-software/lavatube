# Aliasing

## Current support

* Capture stores the backing memory index, binding offset and capture memory requirement
  size for buffers, images and tensors.
* During device initialization, replay uses the capture ranges to find connected groups
  of overlapping objects. Exact, contained and partially overlapping ranges are supported.
* Each group receives one replay allocation. Relative capture offsets are preserved, while
  allocation size, alignment and compatible memory types come from replay requirements.
* Alias allocations are kept until device teardown so destroying one member cannot release
  memory still used by another member.
* Traces captured before the additional binding metadata was added use the legacy
  non-aliasing replay path. They only need to be recaptured if they actually rely on
  aliased memory.

## Test cases

We have a few test cases in `external/tracetooltests/src`:

* `vulkan_aliasing_1.cpp` - binds a 1024-byte parent buffer at offset 0 and two
  non-overlapping 256-byte child buffers at offsets 256 and 512. A GPU copy between
  the children is checked through the parent, covering contained suballocations at
  non-zero offsets and a parent with more than one child.
* `vulkan_aliasing_2.cpp` - binds two buffers at offsets 0 and 512 whose ranges
  partially overlap, but neither contains the other. It verifies that a host write
  through the second range changes the overlapping part of the first range.
* `vulkan_aliasing_3.cpp` - binds two equally sized buffers to exactly the same range
  and verifies that both expose the same contents, covering complete 1:1 overlap.
* `vulkan_compute_aliasing.cpp` - binds a storage buffer and a linear image at offset
  0 in one allocation sized for both resources, then uses the buffer for compute and
  the image for frame-boundary output. With the default Vulkan 1.1 configuration it
  also exercises the core `vkBindBufferMemory2` and `vkBindImageMemory2` entrypoints.

Known gaps in testing coverage:

* There is no image-to-image aliasing test, including images with different creation
  parameters and `VK_IMAGE_CREATE_ALIAS_BIT`, and no tensor aliasing test.
* There is no exact alias at a non-zero offset, group of more than two objects at the
  same offset, multi-level containment, or coverage for different binding orders and
  object lifetimes.
* The KHR binding entrypoints and calls that bind multiple resources in one
  `vkBind*Memory2` invocation are not covered.
* The tests do not deliberately exercise replay requirements with different sizes or
  alignments, nor compatible and incompatible intersections of `memoryTypeBits`.
* All tests select host-visible memory; device-local-only and dedicated or sparse
  aliasing are not covered.

## Plan

* Add tests that exercise the remaining gaps above.
* Abort replay with a clear error if captured relative offsets violate replay alignment,
  no common replay memory type exists, or a member requires a dedicated allocation.
* Consider releasing an alias allocation when its final member is destroyed. The current
  implementation deliberately retains these uncommon allocations until device teardown.

## Open questions

- Do we want to implement the perfect knowledge memory suballocator first?
	- In short, this creates a plan in the replayer for all future non-dedicated memory allocations at initial device creation.
		- Since we have perfect knowledge of the future, we can make more optimal choices in the suballocator and no suballocator overhead after device creation.
	- No. Alias-group planning has been implemented within the current suballocator without
	  pre-planning unrelated allocations.
