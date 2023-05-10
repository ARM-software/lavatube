# Multithreaded design

Vulkan is by design an API meant for multithreaded work. For the most part, it is dependent on the API users to do synchronization. Most of this synchronization is invisible
to the tracer, and it is therefore difficult to reproduce this synchronization correctly and reliably. The way we solve it is by synchronizing access to all Vulkan objects
as they are accessed.

Externally synchronized, definition: "All commands support being called concurrently from multiple threads, but certain parameters, or components of parameters are defined to be externally synchronized. This means that the caller must guarantee that no more than one thread is using such a parameter at a given time."

The design rests of 3 main pillars.

## Object runtime order

All Vulkan objects are stored with a "handle", which consists of the object's index, the thread where it was last touched, and the thread local call number where it was last touched. Before proceeding any further, the replayer checks if this thread has passed this point. If not, it will spinlock until that thread has passed the registered point.

When accessed, every Vulkan command that has external synchronization for a parameter in the Vulkan specification will update the above mentioned object registration for that parameter. This means any other threads running during replay will wait for this point in time before reading the object.

This prevents us from using an object before it is ready and that it contains the last change we need. It also enforces the Vulkan spec's requirement that another thread does not access the externally synchronized object at the same time.

## Local memory barrier

The currently running thread registers a local memory barrier before any function that destroys or resets any Vulkan object. The local memory barrier stores the current position of all other threads in the current thread, and on replay will wait at this point until all other threads have reached the stored positions. This prevents us from destroying or resetting an object before all users are done with it.

This is used for all destroy commands, queue submits, vkUnmapMemory, queue present, and the reset command list (see below).

## Push memory barriers

This type of memory barrier works like the above, except that instead of being added to the local thread it is requested to all other threads after the function is complete. Before the other threads save a new Vulkan command, they will store a local memory barrier. This prevents these other commands from starting before the command issuing the push memory barrier has completed on replay.

This is used for vkWaitForFences and vkGetFenceStatus when these return VK_SUCCESS, vkQueueWaitIdle, vkDeviceWaitIdle, queue present, and the reset command list.

### Reset command list

These commands reset state in a way that is not captured with object runtime order. This list explains which ones and why not:

* vkResetDescriptorPool - to make sure we're not running with old descriptors; this could have been handled with object runtime order except that would introduce a race condition for access to their metadata, since descriptorsets of a pool are not externally synchronized here
* vkResetQueryPool(EXT) - as above but for query pool
* vkResetCommandPool - as above but for commandbuffers; however, all objects in the pool are assumed to be externally synchronized under the "Implicit Externally Synchronized Parameters" rule of the Vulkan spec

Notable candidates:

* vkResetEvent - this should be handled sufficiently by the object runtime order rule
* vkResetFences - this is noted in the spec as an "Externally Synchronized Parameter Lists" so we can assume it has exclusive access to its objects and can use object runtime order rule instead
* vkResetCommandBuffer, vkBeginCommandBuffer and vkEndCommandBuffer - implicitly externally synchronizes the commandpool it is allocated from
* vkUnmapMemory - unmap acts as a flush
* vkFlushMappedMemoryRanges - dependencies between commands and buffer/image update packets
