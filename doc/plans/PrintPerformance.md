# Speeding up lava-print

`lava-print` is currently a bit slow for large trace files, especially when all we want are a
few packets and packet ranges here and there. Can we do better?

## Idea: Separate generated parser code for printing

We currently piggy-back on the replay code for printing, mixing the two. This is not ideal,
but it gave us a quick way to implement the functionality. It also allows us to print threaded calls
in somewhat similar order as a normal replay. If we make separate parser code for printing, we
could optimize it by not using a thread model. The disadvantage is that the thread ordering would
no longer make much sense, and so we should probably only print one thread at a time. The current
thread model also does not accurately portray multi-threaded call ordering, since it does not do
any implementation-side waits, so in that sense it is misleading.

This also allows us to go through the file without incremental state tracking, which in turn will
allow us to jump to any point in the file and print its contents, whereas now we need to build up
the state incrementally or things will break.

We will also need to modify the json helpers, as they currently assume a lot of the state machinery
being initialized and updated.

Questions:
- What else will break / won't be visible if we don't track state incrementally?
- Do we need a new primitive file parser that isn't aggressively multi-threaded? The current
  low-level code in `filereader.h` is optimized for incremental streaming, not for jumping around.

How:
- We would likely need a whole new set of operations in `scripts/util.py` parallel to the current
  'load' and 'save' functions. That would be a lot of new code to maintain, and the reason why we
  don't currently do this.

## Idea: Jump forward indices

Once we have the above, we can start storing the uncompressed position and packet index of the
first packet in its file chunk. This if we are asked to print a particular packet or packet range,
way we can skip compressed chunks until we get to the right one, and then jump ahead inside it to
the first packet, potentially speeding up print operations massively.

We would likely speed things up quite a bit if we had smaller chunks. And also if we stored the
middle packet position and index inside each chunk as well - halving the search space (we could
keep halving it this way but the benefit would likely drop quickly).
