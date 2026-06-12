# VkCommandBuffer splitting

If we could split command buffers on the fly and submit each piece separately,
this would make debugging much easier in some cases, especially when trying to
track down DEVICE_LOST situations.

The naive version is not generally possible. Many commands set command-buffer
state that is discarded when command buffer execution completes. If we simply
ended the current command buffer, submitted it, and started a new empty command
buffer, the later piece would be missing dynamic state, descriptor state,
pipeline state, push constants, render pass state, and other scoped state.

Here is an idea of shadow-commandbuffers to implement this.

* On `vkBeginCommandBuffer`, create a pool of extra replay-only shadow command
  buffers with matching begin flags and inheritance where applicable.
* While replaying each `vkCmd*`, also record selected commands into one or more
  shadow command buffers.
* Commands that establish replay state needed by later pieces are copied into
  future shadow command buffers.
* Commands that produce side effects are recorded only into the current split
  piece.
* At a selected split point, end the current split piece and make the next
  shadow command buffer current.

This feature is replay-only, but we would need to gather some info during
capture to support it.

## The hard part: safe split points

Several command families open or close scopes, mutate both command-buffer state
and external GPU-visible state, or depend on state that cannot be reconstructed
by only copying a single command class.

We now count `renderpass_count` and `shader_command_count` in `trackedcmdbuffer`
during capture, so we know how many we will need. These should exist for all new
traces - perhaps just fail if try to use this feature and we don't have them.

We also have `vk.cmd_scoped_begin_commands` and `vk.cmd_scoped_end_commands` which
we can use to check if we are nested inside a scoped command. For now, we should
not split unless `nesting == 0`.

In `trackedcmdbuffer` also keep `bool instrumented = false` as a master switch for many expensive
operations which we usually skip and do not want to check individually.

In `lava-replay` new options `--cmd-split-by-shader` and `--cmd-split-by-renderpass`.
