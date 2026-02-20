---
name: validate_extension
description: Validate the implementation and support in lavatube for a given Vulkan extension
metadata:
  short-description: Validate lavatube's support for a Vulkan extension
---

1. Check that we were told which Vulkan extension we are supposed to check. If this was not specified, ask.
2. Download the extension text from `https://docs.vulkan.org/refpages/latest/refpages/source/<extension name>.html` and read it.
3. For each new Vulkan command defined in the given extension, look for its generated implementation named
   `trace_<name>` in `generated/write_auto.cpp`. Review it for errors, then compare it to the corresponding
   read function named `retrace_<name>` in `generated/read_auto.cpp`. Verify that the serialized output is
   likely to be read back in correctly. If not found in generated code, they may have been hardcoded in code
   in the `src` directory instead.
4. For each new Vulkan structure defined in the given extension, look for its generated implementation named
   `write_<name>` in `generated/struct_write_auto.cpp`. Review it for errors, then compare it to the
   corresponding read function named `read_<name>` in `generated/struct_read_auto.cpp`. Verify that the
   serialized output is likely to be read back in correctly. If not found in generated code, they may have
   been hardcoded in the `src/hardcode_*.cpp` files instead.
5. Check if any new object types are defined in the given extension. If so, look at the Vulkan creation
   command for the object type, and see if there is any metadata there we should be tracking. If so, that
   should be in `src/lavatube.h` under its corresponding `trackable` child class. The trackable class is
   typically named the same as the object type but in all lower case and its `vk` prefix replaced with
   `tracked`.
6. Check if the extension has been promoted to core (e.g. "Promotion to Vulkan 1.3"). If it has been
   promoted, and we have any hardcoded functions, check if these also have core equivalents (ie function
   name without the extension suffix). Warn if these are missing.
