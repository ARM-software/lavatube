---
name: validate_vulkan_version
description: Validate the implementation and support in lavatube for a given Vulkan version
metadata:
  short-description: Validate lavatube's support for a Vulkan version
---

1. Check that we were told which Vulkan extension we are supposed to check. If this was not specified, ask.
2. If you are not familiar with the extension, download the version summary from
   `https://docs.vulkan.org/refpages/latest/refpages/source/<version>.html`, where `version` is the
   version in the form `VK_VERSION_<major>_<minor>`, for example `VK_VERSION_1_4` for version 1.4.
3. For each new command, check if we have an existing hardcoded KHR or EXT suffixed version of this
   command, and are missing a forward from a promoted command (ie without the suffix). Check the two
   files `src/hardcoded_read.cpp` (for replay) and `src/harcoded_write.cpp` (for capture). Also check
   the file `scripts/vkconfig.py` for extension variants that are similarly missing a promoted variant.
4. As above, for each new structure defined, check if we have an existing hardcoded KHR or EXT suffixed
   version of it and are missing handling of the promoted variant. Look in the same files as for commands.
