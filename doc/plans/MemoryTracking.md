# Changes to memory tracking

The gfxr importer and the forthcoming `VK_KHR_device_address_commands` both
highlight a key weakness in Lavatube design - we track memory by higher level
Vulkan objects only, instead of by the underlying memory objects, like gfxr
does, and once VkBuffer is deprecated this design will become difficult to
maintain.

For now in the importer we maintain a workaround by temporarily keeping
memory from gfxr around as needed. At some point we will need to change this
into a more complete sparse memory clone, either complementing or possibly
replacing the current design.
