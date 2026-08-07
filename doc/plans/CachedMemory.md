# Cached CPU memory during capture

## Motivation

Lavatube detects host writes by reading mapped Vulkan memory during queue
submission. CPU reads from host-visible device-local memory can be extremely
slow even when the application's writes to that memory are fast. In Steel
Nomad, scanning roughly 5.3 GiB from this memory dominated several minutes of
capture time. Using host-cached system memory reduced the same tracking work to
seconds.

Plain snapshot copying does not solve this generally: copying from the
uncached mapping is itself slow. Advertising `VK_MEMORY_PROPERTY_HOST_CACHED_BIT`
on an uncached type would also be incorrect because it does not change the
mapping's cacheability.

The proposed solution is therefore to preserve the memory topology and memory
properties seen by the application, but back selected host-visible allocations
with compatible host-cached CPU memory when calling the driver.

## Goals

- Let the application make its normal memory-type choice from the original
  properties, including `DEVICE_LOCAL` and heap identity.
- Store the type and properties requested by the application in the trace.
- Use host-cached backing memory during capture so memory tracking is fast.
- Keep the requested and actual backing identities explicit in diagnostics and
  tracking metadata.
- Detect unsupported remapping before returning unusable memory requirements.

This initially targets ordinary host-visible buffer and image memory. Imported,
exported, protected, lazily allocated, and other special allocations are out of
scope until individually validated.

## Design

Maintain two identities for each presented memory type:

- **Requested identity:** the index, property flags, and heap presented to and
  selected by the application.
- **Backing identity:** the real host-cached memory type passed to the driver.

`vkGetPhysicalDeviceMemoryProperties*` should continue to present the requested
topology. It must not hide uncached types or add `HOST_CACHED` to them. During
setup, choose a host-cached backing candidate for each ordinary host-visible
requested type. A coherent requested type requires coherent backing; extra
properties on the backing type remain hidden from the application.

Translate every returned `memoryTypeBits` mask. A requested type bit is valid
only when both conditions hold:

1. The driver reported the requested real type as compatible with the object.
2. The driver reported its selected cached backing type as compatible with the
   same object.

This preserves the meaning of the application's selected bit while ensuring
that the allocation substituted later can legally be bound to the object.

When intercepting `vkAllocateMemory`:

1. Serialize and retain the application's original `memoryTypeIndex`.
2. Resolve that requested index to its cached backing index.
3. Pass a copied allocation structure containing the backing index to the
   driver.
4. Track both identities on the `VkDeviceMemory` metadata object.

Bind packets and replay-facing memory flags must describe the requested
identity, not the capture-only backing. Capture metadata should additionally
record the requested and backing indices, flags, and heap indices so the
substitution is visible when debugging.

## Unsupported requirements

If translation turns a nonzero `memoryTypeBits` mask into zero, fail immediately
with the object type, creation flags, original mask, and attempted mappings.
Returning zero leaves the application no allocation-time fallback, while
silently returning the original bit would later substitute an incompatible
backing type.

A future fallback may expose two virtual aliases with the same requested
properties: a preferred alias backed by cached CPU memory and a native-backed
alias. That requires spare memory-type slots and careful handling of allocations
shared by objects with different requirements, so it is deferred until a real
incompatibility requires it.

## Implementation plan

1. Replace the current cached-type filtering table with mappings that separately
   retain requested and backing identities. Keep the feature opt-in while it is
   experimental.
2. Generalize memory-requirement translation for buffers, images, tensors, and
   all core, `KHR`, and `*2` query variants. Translation must cover dedicated
   requirement paths as well as generated callbacks.
3. Remap the copied `VkMemoryAllocateInfo` immediately before the driver call.
   Preserve the untouched structure for serialization and metadata.
4. Extend `trackedmemory` with requested and backing indices, flags, and heap
   indices. Use requested flags for trace packets and replay policy.
5. Add metadata and debug output describing the mapping and allocation sizes per
   type. Add a clear diagnostic for an empty translated mask or an attempted
   unsupported special allocation.
6. Regenerate generated sources with `scripts/lava.py` and add focused tests for
   requirement translation, allocation remapping, nonzero logical type indices,
   and preservation of requested flags.
7. Validate capture and replay separately. Capture should use cached backing;
   replay should continue to allocate according to the application's requested
   properties rather than the capture backing.

## Risks and validation

Forcing CPU memory can change capture-time GPU performance, memory budgets, and
allocation failure behavior. It may also expose driver restrictions for unusual
object or creation-flag combinations. The mode must therefore log actual CPU
heap consumption and initially reject unvalidated special allocation chains.

Validation should include:

- The application observes the same memory properties as without this mode.
- Each translated bit is supported by both requested and backing real types.
- The driver receives the cached backing index while the trace retains the
  requested index and flags.
- Buffers and images can be mapped, bound, submitted, captured, and replayed.
- Aliased and suballocated memory remains valid when several objects share an
  allocation.
- Capture profiling confirms that mapped-memory scanning uses cached backing and
  no longer dominates queue submission.

