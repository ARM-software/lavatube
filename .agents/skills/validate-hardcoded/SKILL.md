---
name: validate-hardcoded
description: Compare lavatube manual Vulkan commands with generated equivalents to find missing shared behavior.
---

# Validate hardcoded commands

Compare the requested commands, or all commands in `hardcoded`, `hardcoded_write`, and `hardcoded_read` in
`scripts/vkconfig.py` when the request is broad. Review without compiling or running tests; make fixes only when
requested.

Generate comparison code in a temporary workspace containing the current generator inputs and required submodule files.
Preserve local edits when copying inputs; generation must not overwrite the active checkout's scripts or `generated/`
output.

In that workspace:

- Remove the selected commands from the applicable hardcoded lists in `scripts/vkconfig.py`.
- Set `bypass_checks = True` in `scripts/util.py` to permit generation of normally manual commands.
- Run `python3 scripts/lava.py` from that workspace's root.

Compare the resulting `trace_<name>` and `retrace_<name>` functions with `src/hardcode_write.cpp` and
`src/hardcode_read.cpp`. Inspect structure serializers when relevant. Look for shared obligations such as capture `thaw`
calls, replay callbacks, handle mapping, metadata, and serialization symmetry.

Generated code is a comparison baseline, not an oracle: manual implementations intentionally differ. Report missing
common behavior with source locations and explain why each difference matters. If isolation is unavailable, report the
limitation and continue with source inspection. Do not use a blanket git reset or checkout to prepare or clean up a
review.
