---
name: validate-extension
description: Audit lavatube capture, replay, and simulator support for a specified Vulkan extension.
---

# Validate a Vulkan extension

Use the extension named in the request or established context; ask only if it is missing or ambiguous. Review without
compiling or running tests. Make fixes only when requested.

Read the current extension specification at `https://docs.vulkan.org/refpages/latest/refpages/source/<extension>.html`;
use the vendored registry and headers to establish what this checkout can generate.

Assess these support boundaries:

- **Commands:** compare `trace_<name>` in `generated/write_auto.cpp` with `retrace_<name>` in `generated/read_auto.cpp`,
  including serialization order and replay behavior. Manual implementations are in `src/hardcode_write.cpp` and
  `src/hardcode_read.cpp`.
- **Structures:** compare `write_<name>` in `generated/struct_write_auto.cpp` with `read_<name>` in
  `generated/struct_read_auto.cpp`, including pointers, counts, and `pNext` handling; follow manual implementations when
  applicable.
- **Objects:** check creation, lifetime, and needed metadata in `src/lavatube.h` and the type maps in
  `scripts/vkconfig.py`.
- **Promotion:** check both extension and core aliases, especially forwarding to manual implementations.
- **Shader support:** if the extension introduces SPIR-V requirements, inspect
  `external/spirv-simulator/framework/spirv_simulator.cpp` and its relevant helpers.
- **Coverage:** find uses in `tests/` and `external/tracetooltests`; report missing coverage separately from
  implementation defects.

If generated files are absent, report that limitation or use an isolated generation workspace; do not alter the active
checkout just to perform the review. Report concrete findings with source locations and distinguish inspected support
from runtime verification.
