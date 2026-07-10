# Compressed assets file

Motivation: To speed up tracing and replay, we could store and restore constant
already compressed assets in a separate file that we leave uncompressed. We
just refer into it by index and size when we need something.

The separation of trace file vs asset file would be that trace file contains
API calls, while asset file contains binary blobs of data usually captured from
mapped memory.

## Capture time async write idea

Whenever we have a vkCmdCopyBufferToImage(?) etc being executed, we can know that
the origin is compressed image data. We can allocate space for it and start an
async stream of its data into the assets file. As it is uncompressed, we can
calculate the future position of our data in the assets file well in advance of
the actual writing of the data.

Since the resource could be reused after queue submit, we would need to block our
stream on whatever waits for the results of the submit. This means injecting a
synchronization for it into wait queue, wait device, fence status and event commands.

Probably not good to have multiple async streams into the asset file at the same
time. Could just have one worker thread picking up write requests.

If the app has a queue submit for each resource staging, we could still get blocked
a lot, and perhaps not have any gains. We _could_ do a CoW on the memory, possibly,
but this seems risky.

## Sharing idea

One extension idea for this is to share an asset file between multiple traces. This
could save significant amounts of disk space. To do this, we would have to refer to
entries by hash rather than index, as each user of the asset file may have its
assets in slightly different order and may add/omit some assets the other might use.

One way to implement this would be to have one trace _inherit_ asset state from
another trace. Then the child trace could have its own asset file with any assets
not in its parent asset file.

All ways of sharing would require either having the older trace available during
capture of a new trace, or carry out the sharing during postprocessing. On-the-fly
sharing would be a bit more complex, but could speed up capture (we could possibly
even use it just to speed up slow captures by doing a capture in two stages, but
assumes write-out is significantly slower than hashing).

## Metadata idea

To make the asset file more generally useful (eg for microbenchmarks, inspection,
debugging, or asset replacement), we should also store metadata for each asset,
ideally enough to allow usage of the asset without the originating trace file.
It could also be used to rewrite the assets (eg changing compression or dimensions)
then use these in the trace file, but this would require possibly quite complex
changes of the replay, depending on the type of resource.
