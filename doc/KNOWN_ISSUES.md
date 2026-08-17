For mutable descriptors:

- src/tool.cpp resolves mutable descriptor types by global append order, not the type active at that command. Reusing a slot as uniform → storage → uniform with identical bytes leaves storage selected, causing write-out to emit the wrong
  concrete descriptor and potentially corrupt cross-device replay.
- src/execute_commands.cpp guesses mutable-array stride from observed payload sizes and binding span. Vulkan defines it as the maximum descriptor size allowed by the binding’s mutable type list, which the tracked layout does not retain.
  Padding or unused larger types can therefore resolve the wrong array element. See the Vulkan descriptor-buffer rules (https://docs.vulkan.org/spec/latest/chapters/descriptorbuffers.html).
- src/tool.cpp reconstructs array offsets from all historical payload observations for that buffer range. If the region was previously used with another layout/stride, matching stale offsets are sorted and truncated to descriptorCount,
  potentially excluding a currently valid later element.

