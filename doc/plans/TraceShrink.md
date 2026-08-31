# Shrinking trace files

Motivation: We want to add more trace files to our regular testing, and we have
many samples and demos we could add, but they tend to be too large. If we could
shrink them, we grow our testing coverage.

## 1. Identify what consumes size in traces

Create option `lava-tool --space-usage <trace file>` that breaks down what a
trace file's file size goes into. We should at least identify images,
acceleration structures, image sources (in buffers), other buffer content,
and call/packet serialization. Then we know how to best shrink them.

This is now done.

For a more detailed view of the image-source category, use
`lava-tool -iu/--image-usage <trace file>`. This reports the serialized buffer
update bytes by destination `VkFormat`. If one buffer supplies images with more
than one format, its bytes are shown once under a combined format label.

## 2. Shrink images

I suspect textures are the number one consumer of space. We can identify where
they are created and shrink them there, then shrink their associated buffer and
image update packets. The key problem is identifying staging buffers used to
copy the initial image data into an image. We can find them by looking at eg
`vkCmdCopyBufferToImage` calls being executed and know that the origin is
image data.

We will need some kind of image parsing library to be able to shrink the actual
image data.

## 3. Shrink acceleration structures

TBD. Probably very difficult?

## Interface

`lava-tool --shrink <factor> <input trace file> <output trace file>`

Where `<factor>` would be eg 2 for shrinking textures by half.
