# Plan for next file format update

Motivation: We want to make a bunch of breaking changes at once, as this will
cause all existing traces to break, and we would rather not do this very often.

## Old traces

We do not need a porting strategy for existing traces - I will re-create them.
Re-running `ctest` will re-create most traces used in our CI testing. The
exception are the traces in the `traces` folder, which I will re-create and
replace manually - ignore any ctest errors from these until I have replaced them.

## Code

We want to get rid of any code that is no longer needed after these changes.
Anything gated behind version checks should be removed.

## The changes

- Remove all code lines in `scripts/util.py` and `src/tool.cpp` marked with
  "TBD remove". This intentionally includes the obsolete memory flags, image
  tiling, and padded image size fields added to the bind-memory packet format.
  Remove each field from both the generated writer and reader code at the same
  time. When removing the delayed image-size callbacks in `src/tool.cpp`,
  restore direct image-size synchronization by assigning `writer_image->size`
  from `reader_image.size` in `sync_output_image_memory_metadata()`.
