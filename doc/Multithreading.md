# Multithreaded design

Vulkan is by design an API meant for multithreaded work. For the most part, it is
dependent the users of its API to do thread synchronization. Most of this
synchronization work is invisible to an API tracer, and it is therefore difficult
to reproduce it correctly and reliably. The way we solve it in lavatube is by
synchronizing access to all Vulkan objects the moment that they are accessed and
by inserting strategically placed thread barriers.

We rely on the Vulkan standard's requirement that the API user handles
synchronization of any API objects defined as `externally synchronized` in a
certain way. From the standards text:

> "All commands support being called concurrently from multiple threads, but certain
> parameters, or components of parameters are defined to be externally synchronized.
> This means that the caller must guarantee that no more than one thread is using
> such a parameter at a given time."

For the most part we rely on this guarantee to provide multi-threading support.

## Object runtime order

All Vulkan objects inside lavatube are stored with a handle which consists of the
object's index as well as the index of the thread and thread local call number
where it was last touched during tracing. Whenever a handle is encountered, the
replayer will verify that the thread that last touched this handle has passed this
recorded point in time, and if not, we will spin lock until the other thread has
passed it.

During tracing, every object that is accessed where the standard specifies that it
requires `external synchronization`, and few extra ones that do not, will be marked
as 'touched' for the purpose of such synchronization during replay. This reproduces
a minimal ordering of thread accesses such that original app behaviour and replay
behaviour both are correctly synchronized but without imposing so much
synchronization that performance is ruined.

## Local memory barrier

The currently running thread registers a local memory barrier before any function
that destroys or resets any Vulkan object. The local memory barrier stores the
current position of all other threads in the current thread, and on replay will
wait at this point until all other threads have reached the stored positions.
This prevents us from destroying or resetting an object before all users are done
with it.

This is used before all destroy commands, pool reset commands, queue submits,
memory unmaps, and queue presents.

For example, we cannot destroy a Vulkan object without being sure that no other
thread will use it, and the only feasible way of making sure of this is to wait
until all other threads are at or past the point they were during recording.

## Push memory barriers

Sometimes just making sure that the current thread is past a safe point is not
enough, and we need to make sure all threads are correctly ordered before
continuing. Push memory barriers is what we use for this. These are added at
the end of a Vulkan command to instruct all other threads to insert a local
memory barrier before their next Vulkan command. This prevents these other
commands from starting before the command issuing the push memory barrier has
completed on replay.

This is used for commands such as vkWaitForFences and vkGetFenceStatus when these
return VK_SUCCESS, vkQueueWaitIdle, vkDeviceWaitIdle, queue presents, and pool
reset commands.

This is needed when a command on another thread may require a certain state to
be reset before it can safely be run, and the object runtime order does not give
us enough information to make this guarantee.

## The sync mutex

Lavatube also has a special mutex around a few Vulkan commands:
vkQueueSubmit, vkQueueSubmit2, vkQueueWaitIdle, vkQueueBindSparse, and
vkDestroyDevice

This extra synchronization is not needed for multithreading support, but
rather for increased flexibility of virtualization and portability. Once these
commands are serialized, we can rewrite the way we submit jobs across queues
and present virtual queues to the application.

## Special considerations

### vkResetDescriptorPool and vkResetQueryPool(EXT) and vkResetCommandPool

Need to add both local and push barriers to make sure other threads are not
replaying with old objects. This could have been handled with object runtime
order except that would introduce a race condition for access to their metadata,
since objects of a pool are not externally synchronized.

Athough For command pools, all objects in the pool are assumed to be externally
synchronized under the "Implicit Externally Synchronized Parameters" rule of
the Vulkan spec.

### vkResetEvent

This should be handled sufficiently by the object runtime order rule.

### vkResetFences

This is noted in the spec as an "Externally Synchronized Parameter Lists" so we
can assume it has exclusive access to its objects and can use object runtime
order rule instead.

### vkUnmapMemory and vkFlushMappedMemoryRanges

Here we have dependencies between commands and buffer/image host updates, so we
need to introduce a local thread barrier.

### vkResetCommandBuffer, vkBeginCommandBuffer and vkEndCommandBuffer

We need to make sure these are explicitly externally synchronized.
