# Problems with counting commands

Counting commands here refer to any command in Vulkan that are run twice, first
to get the item count, and then again to fill a pointer with counted data. This
is a very frequently used pattern. We currently recognize 41 such commands.

## Issues

- The trace path does not serialize the actual outputs for these commands. During capture,
  the special path for such commands only writes a single boolean in scripts/util.py, then
  the normal output save path is skipped. That means no captured count value, no captured
  array contents, and no captured handles for special-count queries. The reason for this
  originally was because the code generator was pretty bad and this made things simpler.
  But also, the data is never actually used during replay in most cases.
- The problem with changing this now is that it would break backwards trace compatibility.
  We could solve this by incrementing the file version and checking it before reading the
  extra data, though.
- The replay path also skips normal post-load handling and callbacks for all such commands.
  So any tracking or remapping that depends on loaded outputs never runs for these commands.
- The generator collapses all output-pointer presence into a single boolean. That is wrong
  for commands like the performance-counter queries, where the outputs can be independently
  optional. (Although there is currently only one of these right now.)
- Codex insists during reviews that `VK_INCOMPLETE` is not handled during replay. I do not
  think this is a real issue but it would be nice to shut it up about it.

## What we should fix

These are things it would be nice to fix:

- Make post-callbacks work.
- Make codex stop complaining.

## Codex comments

There is an even simpler point: for most Vulkan “call twice” commands, you do not need a preliminary
live count query just to avoid overflow. You can allocate a temporary live buffer using stored_count,
set *pCount = stored_count, and call the function once:

  - if live_count > stored_count, you get VK_INCOMPLETE, which you already said is acceptable
  - if live_count < stored_count, the buffer is still large enough, so no overflow

## Plan

