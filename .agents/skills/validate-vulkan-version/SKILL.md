---
name: validate-vulkan-version
description: Audit lavatube support for a specified core Vulkan version, including promotions and feature pruning.
---

# Validate a Vulkan version

Use the core version named in the request or established context; ask only if it is missing or ambiguous. Review without
compiling or running tests. Make fixes only when requested.

Read `https://docs.vulkan.org/refpages/latest/refpages/source/VK_VERSION_<major>_<minor>.html` for the current version
summary. Compare it with the vendored Vulkan registry and headers to identify checkout limitations.

- Check new commands and structures for capture/replay serialization symmetry in `generated/*_auto.cpp` and manual
  handling in `src/hardcode_read.cpp` and `src/hardcode_write.cpp`.
- Check promoted KHR/EXT commands and structures in both directions: core aliases must reach the intended
  implementation, and extension variants must retain equivalent handling. Inspect `scripts/vkconfig.py` as well as
  manual code.
- Follow feature recording and pruning through `scripts/lava.py` (`extra_tracked_structs`), `src/write.cpp`,
  `src/hardcode_read.cpp`, and `external/tracetooltests/src/usagetracker/`. Check how required/removed features are
  stored in trace metadata and applied during replay.
- Locate version-specific tests in `tests/` and `external/tracetooltests`; distinguish missing coverage from proven
  defects.

If generated files are absent, report that limitation or use an isolated generation workspace. Report findings with
source locations and identify any support that remains unverified at runtime.
