# Compressed assets file

Motivation: To speed up tracing and replay, we could store and restore constant
already compressed assets from a separate file that we leave uncompressed. We
just refer into it by index and size when we need something.

## Detection

Whenever we have a vkCmdCopyBufferToImage(?) etc, we can know that the origin is
compressed image data. We can allocate space for it and start an async stream of
its data into the assets file. As it is uncompressed, we can calculate the future
position of our data in the assets file well in advance of the actual writing of
the data.

Since the resource could be reused after queue submit, we would need to block our
stream on whatever waits for the results of the submit. This means injecting a
synchronization for it into wait queue, wait device, fence status and event commands.

Probably not good to have multiple async streams into the asset file at the same
time. Could just have one worker thread picking up write requests.

If the app has a queue submit for each resource staging, we could still get blocked
a lot, and perhaps not have any gains. We _could_ do a CoW on the memory, possibly,
but risky.
