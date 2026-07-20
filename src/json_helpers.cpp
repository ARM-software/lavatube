// --- JSON write helpers ---
// We only write out static info that does not change for the duration of the trace.

#include "jsoncpp/json/writer.h"

#include "json_helpers.h"

static bool valid_change_source(const change_source& c)
{
	return c.packet != UINT32_MAX && c.frame != UINT32_MAX && c.thread != UINT8_MAX && (c.packet_type != UINT8_MAX || c.call_id != UINT16_MAX);
}

static const char* trackable_state_string(const trackable* t)
{
	switch (t->state)
	{
	case (uint8_t)trackable::states::uninitialized: return "uninitialized";
	case (uint8_t)trackable::states::initialized: return "initialized";
	case (uint8_t)trackable::states::created: return "created";
	case (uint8_t)trackable::states::destroyed: return "destroyed";
	case (uint8_t)trackedobject::states::bound: return "bound";
	default: return "unknown";
	}
}

static void strip_show_meta_lifecycle(Json::Value& meta)
{
	static const char* keys[] =
	{
		"frame_created",
		"packet_created",
		"thread_created",
		"api_created",
		"frame_destroyed",
		"packet_destroyed",
		"thread_destroyed",
		"api_destroyed",
		"name",
	};
	for (const char* key : keys)
	{
		meta.removeMember(key);
	}
}

Json::Value trackable_json(const trackable* t)
{
	Json::Value v;
	assert(t->creation.frame != UINT32_MAX);
	v["frame_created"] = t->creation.frame;
	v["packet_created"] = t->creation.packet; // local packet number
	v["thread_created"] = t->creation.thread;
	v["api_created"] = t->creation.call_id;
	if (t->destroyed.frame != UINT32_MAX)
	{
		v["frame_destroyed"] = t->destroyed.frame;
		v["packet_destroyed"] = t->destroyed.packet;
		v["thread_destroyed"] = t->destroyed.thread;
		v["api_destroyed"] = t->destroyed.call_id;
	}
	if (!t->name.empty()) v["name"] = t->name;
	return v;
}

Json::Value cli_show_object_json(const char* object_type, const trackable* t, Json::Value meta)
{
	strip_show_meta_lifecycle(meta);

	Json::Value v;
	v["type"] = object_type;
	v["index"] = t->index;
	v["state"] = trackable_state_string(t);
	if (!t->name.empty()) v["name"] = t->name;
	if (valid_change_source(t->creation)) v["creation"] = from_change_source(t->creation);

	const change_source& last_modified = valid_change_source(t->last_modified) ? t->last_modified : t->creation;
	if (valid_change_source(last_modified)) v["last_modified"] = from_change_source(last_modified);
	if (valid_change_source(t->destroyed)) v["destroyed"] = from_change_source(t->destroyed);
	v["meta"] = meta;
	return v;
}

static Json::Value trackedobject_json(const trackedobject *t)
{
	Json::Value v = trackable_json(t);
	v["size"] = (Json::Value::UInt64)t->size;
	v["parent_device_index"] = t->parent_device_index;
	if (t->device_address != 0)
	{
		v["device_address"] = (Json::Value::UInt64)t->device_address;
	}
	if (t->alias_index != UINT32_MAX)
	{
		v["alias_index"] = t->alias_index;
		v["alias_type"] = (unsigned)t->alias_type;
	}
	if (t->backing_index != UINT32_MAX)
	{
		v["backing_memory_index"] = t->backing_index;
		v["memory_offset"] = (Json::Value::UInt64)t->offset;
	}
	v["req_size"] = (Json::Value::UInt64)t->req.size;
	v["req_alignment"] = (unsigned)t->req.alignment;
	v["memory_flags"] = (unsigned)t->memory_flags;
	v["tiling"] = (unsigned)t->tiling;
	return v;
}

Json::Value trackedaccelerationstructure_json(const trackedaccelerationstructure* t)
{
	Json::Value v = trackedobject_json(t);
	v["offset"] = (Json::Value::UInt64)t->offset;
	v["buffer_index"] = t->buffer_index;
	v["flags"] = (unsigned)t->flags;
	return v;
}

Json::Value trackedbuffer_json(const trackedbuffer* t)
{
	Json::Value v = trackedobject_json(t);
	v["flags"] = (unsigned)t->flags;
	v["sharingMode"] = (unsigned)t->sharingMode;
	v["usage"] = (unsigned)t->usage;
	v["usage2"] = (Json::Value::UInt64)t->usage2;
	v["written"] = (Json::Value::UInt64)t->written;
	v["updates"] = (unsigned)t->updates;
	return v;
}

Json::Value trackedtensor_json(const trackedtensor* t)
{
	Json::Value v = trackedobject_json(t);
	v["flags"] = (Json::Value::UInt64)t->flags;
	v["sharingMode"] = (unsigned)t->sharingMode;
	v["format"] = (unsigned)t->format;
	v["usage"] = (unsigned)t->usage;
	v["dimensions"] = Json::arrayValue;
	for (unsigned i = 0; i < t->dimensions.size() ; i++)
	{
		v["dimensions"].append((Json::Value::UInt64)t->dimensions.at(i));
	}
	if (t->strides.size() > 0) v["strides"] = Json::arrayValue;
	for (unsigned i = 0; i < t->strides.size() ; i++)
	{
		v["strides"].append((Json::Value::UInt64)t->strides.at(i));;
	}
	return v;
}

Json::Value trackedtensorview_json(const trackedtensorview* t)
{
	Json::Value v = trackable_json(t);
	v["tensor"] = (unsigned)t->tensor_index;
	v["format"] = (unsigned)t->format;
	v["flags"] = (Json::Value::UInt64)t->flags;
	return v;
}

Json::Value trackeddatagraphpipelinesession_json(const trackeddatagraphpipelinesession* t)
{
	Json::Value v = trackedobject_json(t);
	v["flags"] = (Json::Value::UInt64)t->flags;
	v["pipeline_index"] = t->pipeline_index;
	return v;
}

Json::Value trackedimage_json(const trackedimage* t)
{
	Json::Value v = trackedobject_json(t);
	v["flags"] = (unsigned)t->flags;
	v["sharingMode"] = (unsigned)t->sharingMode;
	v["usage"] = (unsigned)t->usage;
	v["imageType"] = (unsigned)t->imageType;
	v["format"] = (unsigned)t->format;
	v["written"] = (Json::Value::UInt64)t->written;
	v["updates"] = (unsigned)t->updates;
	Json::Value arr = Json::arrayValue;
	arr.append((unsigned)t->extent.width);
	arr.append((unsigned)t->extent.height);
	arr.append((unsigned)t->extent.depth);
	v["extent"] = arr;
	v["initalLayout"] = (unsigned)t->initialLayout;
	v["samples"] = (unsigned)t->samples;
	v["mipLevels"] = (unsigned)t->mipLevels;
	v["arrayLayers"] = (unsigned)t->arrayLayers;
	if (t->is_swapchain_image) v["swapchain_image"] = true;
	return v;
}

Json::Value trackedswapchain_json(const trackedswapchain* t)
{
	Json::Value v = trackable_json(t);
	v["imageFormat"] = (unsigned)t->info.imageFormat;
	v["imageUsage"] = (unsigned)t->info.imageUsage;
	v["imageSharingMode"] = (unsigned)t->info.imageSharingMode;
	v["width"] = (unsigned)t->info.imageExtent.width;
	v["height"] = (unsigned)t->info.imageExtent.height;
	return v;
}

Json::Value trackedcmdbuffer_json(const trackedcmdbuffer* t)
{
	Json::Value v = trackable_json(t);
	if (t->device_index != CONTAINER_INVALID_INDEX) v["parent_device_index"] = t->device_index;
	if (t->pool_index != CONTAINER_INVALID_INDEX) v["pool"] = t->pool_index;
	if (t->replay_submit_fence_index != CONTAINER_INVALID_INDEX) v["replay_submit_fence_index"] = t->replay_submit_fence_index;
	if (t->bound_raytracing_pipeline_index != CONTAINER_INVALID_INDEX) v["bound_raytracing_pipeline_index"] = t->bound_raytracing_pipeline_index;
	v["commands"] = (unsigned)t->commands.size();
	v["replay_pending"] = t->replay_pending;
	v["renderpass_count"] = t->renderpass_count;
	v["shader_command_count"] = t->shader_command_count;
	return v;
}

Json::Value trackedcmdbuffer_trace_json(const trackedcmdbuffer_trace* t)
{
	Json::Value v = trackable_json(t);
	v["pool"] = (unsigned)t->pool_index;
	v["renderpass_count"] = t->renderpass_count;
	v["shader_command_count"] = t->shader_command_count;
	return v;
}

Json::Value trackedimageview_json(const trackedimageview* t)
{
	Json::Value v = trackable_json(t);
	v["image"] = (unsigned)t->image_index;
	return v;
}

Json::Value trackedbufferview_json(const trackedbufferview* t)
{
	Json::Value v = trackable_json(t);
	v["buffer"] = (unsigned)t->buffer_index;
	return v;
}

Json::Value trackeddescriptorset_json(const trackeddescriptorset* t)
{
	Json::Value v = trackable_json(t);
	if (t->pool_index != CONTAINER_INVALID_INDEX) v["pool"] = t->pool_index;
	v["bound_buffers"] = (unsigned)t->bound_buffers.size();
	v["bound_images"] = (unsigned)t->bound_images.size();
	v["bound_opaque_descriptors"] = (unsigned)t->bound_opaque_descriptors.size();
	v["dynamic_buffers"] = (unsigned)t->dynamic_buffers.size();
	return v;
}

Json::Value trackeddescriptorset_trace_json(const trackeddescriptorset_trace* t)
{
	Json::Value v = trackable_json(t);
	v["pool"] = (unsigned)t->pool_index;
	return v;
}

Json::Value trackedqueue_json(const trackedqueue* t)
{
	Json::Value v = trackable_json(t);
	v["queueFamily"] = (unsigned)t->queueFamily;
	v["queueIndex"] = (unsigned)t->queueIndex;
	v["queueFlags"] = (unsigned)t->queueFlags;
	v["parent_device_index"] = (unsigned)t->device_index;
	return v;
}

Json::Value trackedevent_trace_json(const trackedevent_trace* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackedmemory_json(const trackedmemory* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackedfence_json(const trackedfence* t)
{
	Json::Value v = trackable_json(t);
	v["flags"] = (unsigned)t->flags;
	return v;
}

Json::Value trackedsemaphore_json(const trackedsemaphore* t)
{
	return trackable_json(t);
}

Json::Value trackedpipeline_json(const trackedpipeline* t)
{
	Json::Value v = trackable_json(t);
	v["flags"] = (unsigned)t->flags;
	v["type"] = (unsigned)t->type;
	return v;
}

Json::Value trackedcommandpool_trace_json(const trackedcommandpool_trace* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackeddescriptorpool_trace_json(const trackeddescriptorpool_trace* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackeddevice_json(const trackeddevice* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackedshadermodule_json(const trackedshadermodule* t)
{
	Json::Value v = trackable_json(t);
	if (t->enables_device_address) v["enables_device_address"] = true;
	v["size"] = (unsigned)t->size;
	return v;
}

Json::Value trackedphysicaldevice_json(const trackedphysicaldevice* t)
{
	Json::Value v = trackable_json(t);
	v["deviceType"] = (unsigned)t->deviceType;
	return v;
}

Json::Value trackedframebuffer_json(const trackedframebuffer* t)
{
	Json::Value v = trackable_json(t);
	v["width"] = t->width;
	v["height"] = t->height;
	v["layers"] = t->layers;
	return v;
}

Json::Value trackedrenderpass_json(const trackedrenderpass* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackedindirectexecutionset_json(const trackedindirectexecutionset* t)
{
	Json::Value v = trackable_json(t);
	v["type"] = (unsigned)t->type;
	return v;
}

Json::Value trackedindirectcommandslayout_json(const trackedindirectcommandslayout* t)
{
	Json::Value v = trackable_json(t);
	v["flags"] = (unsigned)t->flags;
	v["stages"] = (unsigned)t->stages;
	v["indirect_stride"] = (unsigned)t->indirectStride;
	if (t->pipeline_layout_index != CONTAINER_INVALID_INDEX) v["pipeline_layout_index"] = t->pipeline_layout_index;
	return v;
}

Json::Value trackedpipelinelayout_json(const trackedpipelinelayout* t)
{
	Json::Value v = trackable_json(t);
	v["push_constant_space_used"] = t->push_constant_space_used;
	if (!t->layout_indices.empty())
	{
		v["layout_indices"] = Json::arrayValue;
		for (uint32_t layout_index : t->layout_indices)
		{
			v["layout_indices"].append(layout_index);
		}
	}
	return v;
}

Json::Value trackeddescriptorsetlayout_json(const trackeddescriptorsetlayout* t)
{
	Json::Value v = trackable_json(t);
	return v;
}

Json::Value trackeddescriptorupdatetemplate_json(const trackeddescriptorupdatetemplate* t)
{
	Json::Value v = trackable_json(t);
	v["template_type"] = t->type;
	v["create_flags"] = t->flags;
	v["data_size"] = t->data_size;
	return v;
}

Json::Value trackedsurface_json(const trackedsurface* t)
{
	Json::Value v = trackable_json(t);
	v["width"] = t->width;
	v["height"] = t->height;
	v["x"] = t->x;
	v["y"] = t->y;
	return v;
}

Json::Value trackedshaderobject_json(const trackedshaderobject* t)
{
	Json::Value v = trackable_json(t);
	v["flags"] = t->stage.flags;
	v["stage"] = t->stage.stage;
	v["entry_name"] = t->stage.name;
	return v;
}

// --- JSON read helpers ---

static void trackable_helper(trackable& t, const Json::Value& v)
{
	t.index = v["index"].asUInt();
	t.creation.frame = v["frame_created"].asUInt();
	if (v.isMember("packet_created")) t.creation.packet = v["packet_created"].asUInt();
	if (v.isMember("thread_created")) t.creation.thread = v["thread_created"].asUInt();
	if (v.isMember("api_created")) t.creation.call_id = v["api_created"].asUInt();
	if (v.isMember("frame_destroyed")) // check for legacy value of -1
	{
		if (v["frame_destroyed"].type() == Json::intValue && v["frame_destroyed"].asInt() == -1) { t.destroyed.frame = UINT32_MAX; }
		else t.destroyed.frame = v["frame_destroyed"].asUInt();
	}
	if (v.isMember("packet_destroyed")) t.destroyed.packet = v["packet_destroyed"].asUInt();
	if (v.isMember("thread_destroyed")) t.destroyed.thread = v["thread_destroyed"].asUInt();
	if (v.isMember("api_destroyed")) t.destroyed.call_id = v["api_destroyed"].asUInt();
	if (v.isMember("name")) t.name = v["name"].asString();
}

trackable trackable_json(const Json::Value& v)
{
	trackable t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackedfence trackedfence_json(const Json::Value& v)
{
	trackedfence t;
	trackable_helper(t, v);
	t.flags = v["flags"].asInt();
	t.enter_initialized();
	return t;
}

trackedsemaphore trackedsemaphore_json(const Json::Value& v)
{
	trackedsemaphore t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackedpipeline trackedpipeline_json(const Json::Value& v)
{
	trackedpipeline t;
	trackable_helper(t, v);
	t.flags = v["flags"].asUInt();
	t.type = (VkPipelineBindPoint)v["type"].asUInt();
	t.enter_initialized();
	return t;
}

trackedaccelerationstructure trackedaccelerationstructure_json(const Json::Value& v)
{
	trackedaccelerationstructure t;
	trackable_helper(t, v);
	t.flags = (VkAccelerationStructureCreateFlagsKHR)v["flags"].asUInt();
	t.size = (VkDeviceSize)v["size"].asUInt64();
	t.offset = (VkDeviceSize)v["offset"].asUInt64();
	t.buffer_index = v["buffer_index"].asUInt();
	if (v.isMember("device_address")) t.capture_device_address = v["device_address"].asUInt64();
	t.object_type = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
	if (v.isMember("alias_index"))
	{
		t.alias_index = v["alias_index"].asUInt();
		t.alias_type = (VkObjectType)v["alias_type"].asUInt();
	}
	if (v.isMember("memory_flags")) t.memory_flags = (VkMemoryPropertyFlags)v["memory_flags"].asUInt();

	if (v.isMember("parent_device_index")) t.parent_device_index = v["parent_device_index"].asUInt();
	else t.parent_device_index = 0; // use a default for old trace files, and pray we only have one VkDevice

	t.enter_initialized();
	return t;
}

trackedbuffer trackedbuffer_json(const Json::Value& v)
{
	trackedbuffer t;
	trackable_helper(t, v);
	t.size = (VkDeviceSize)v["size"].asUInt64();
	t.flags = (VkBufferCreateFlags)v["flags"].asUInt();
	t.sharingMode = (VkSharingMode)v["sharingMode"].asUInt();
	t.usage = (VkBufferUsageFlags)v["usage"].asUInt();
	if (v.isMember("usage2")) t.usage2 = (VkBufferUsageFlags2)v["usage2"].asUInt64();
	if (v.isMember("device_address")) t.capture_device_address = v["device_address"].asUInt64();
	t.object_type = VK_OBJECT_TYPE_BUFFER;
	if (v.isMember("alias_index"))
	{
		t.alias_index = v["alias_index"].asUInt();
		t.alias_type = (VkObjectType)v["alias_type"].asUInt();
	}
	if (v.isMember("memory_flags")) t.memory_flags = (VkMemoryPropertyFlags)v["memory_flags"].asUInt();
	if (v.isMember("backing_memory_index"))
	{
		t.backing_index = v["backing_memory_index"].asUInt();
		t.offset = v["memory_offset"].asUInt64();
		t.req.size = v["req_size"].asUInt64();
		t.req.alignment = v["req_alignment"].asUInt64();
	}

	if (v.isMember("parent_device_index")) t.parent_device_index = v["parent_device_index"].asUInt();
	else t.parent_device_index = 0; // use a default for old trace files, and pray we only have one VkDevice

	t.enter_initialized();
	return t;
}

trackedimage trackedimage_json(const Json::Value& v)
{
	trackedimage t;
	trackable_helper(t, v);
	t.size = (VkDeviceSize)v.get("size", 0).asUInt64();
	t.tiling = (lava_tiling)v["tiling"].asUInt();
	t.flags = (VkImageCreateFlags)v["flags"].asUInt();
	t.sharingMode = (VkSharingMode)v["sharingMode"].asUInt();
	t.usage = (VkImageUsageFlags)v["usage"].asUInt();
	t.imageType = (VkImageType)v["imageType"].asUInt();
	t.initialLayout = (VkImageLayout)(v.get("initialLayout", 0).asUInt());
	t.currentLayout = t.initialLayout;
	t.samples = (VkSampleCountFlagBits)(v.get("samples", 0).asUInt());
	t.mipLevels = (unsigned)v.get("mipLevels", 0).asUInt();
	t.arrayLayers = (unsigned)v.get("arrayLayers", 0).asUInt();
	t.format = (VkFormat)v.get("format", VK_FORMAT_MAX_ENUM).asUInt();
	if (v.isMember("device_address")) t.capture_device_address = v["device_address"].asUInt64();
	if (v.isMember("extent"))
	{
		t.extent.width = v["extent"][0].asUInt();
		t.extent.height = v["extent"][1].asUInt();
		t.extent.depth = v["extent"][2].asUInt();
	}
	if (v.isMember("alias_index"))
	{
		t.alias_index = v["alias_index"].asUInt();
		t.alias_type = (VkObjectType)v["alias_type"].asUInt();
	}
	if (v.isMember("memory_flags")) t.memory_flags = (VkMemoryPropertyFlags)v["memory_flags"].asUInt();
	if (v.isMember("backing_memory_index"))
	{
		t.backing_index = v["backing_memory_index"].asUInt();
		t.offset = v["memory_offset"].asUInt64();
		t.req.size = v["req_size"].asUInt64();
		t.req.alignment = v["req_alignment"].asUInt64();
	}
	if (v.isMember("swapchain_image")) t.is_swapchain_image = v["swapchain_image"].asBool();
	t.object_type = VK_OBJECT_TYPE_IMAGE;

	if (v.isMember("parent_device_index")) t.parent_device_index = v["parent_device_index"].asUInt();
	else t.parent_device_index = 0; // use a default for old trace files, and pray we only have one VkDevice

	t.enter_initialized();
	return t;
}

trackedtensor trackedtensor_json(const Json::Value& v)
{
	trackedtensor t;
	trackable_helper(t, v);
	t.size = (VkDeviceSize)v.get("size", 0).asUInt64();
	t.flags = (VkTensorCreateFlagsARM)v["flags"].asUInt64();
	t.sharingMode = (VkSharingMode)v["sharingMode"].asUInt();
	t.object_type = VK_OBJECT_TYPE_TENSOR_ARM;
	t.tiling = (lava_tiling)v["tiling"].asUInt();
	t.format = (VkFormat)v["format"].asUInt();
	t.usage = (VkTensorUsageFlagsARM)v.get("usage", 0).asUInt64();
	if (v.isMember("device_address")) t.capture_device_address = v["device_address"].asUInt64();
	for (const auto& val : v["dimensions"]) t.dimensions.push_back(val.asUInt64());
	if (v.isMember("strides"))
	{
		for (const auto& val : v["strides"]) t.strides.push_back(val.asUInt64());
	}
	if (v.isMember("alias_index"))
	{
		t.alias_index = v["alias_index"].asUInt();
		t.alias_type = (VkObjectType)v["alias_type"].asUInt();
	}
	if (v.isMember("memory_flags")) t.memory_flags = (VkMemoryPropertyFlags)v["memory_flags"].asUInt();
	if (v.isMember("backing_memory_index"))
	{
		t.backing_index = v["backing_memory_index"].asUInt();
		t.offset = v["memory_offset"].asUInt64();
		t.req.size = v["req_size"].asUInt64();
		t.req.alignment = v["req_alignment"].asUInt64();
	}

	if (v.isMember("parent_device_index")) t.parent_device_index = v["parent_device_index"].asUInt();
	else t.parent_device_index = 0; // use a default for old trace files, and pray we only have one VkDevice

	t.enter_initialized();
	return t;
}

trackedtensorview trackedtensorview_json(const Json::Value& v)
{
	trackedtensorview t;
	trackable_helper(t, v);
	t.tensor_index = v["tensor"].asUInt();
	t.format = (VkFormat)v.get("format", VK_FORMAT_UNDEFINED).asUInt();
	t.flags = (VkTensorViewCreateFlagsARM)v.get("flags", 0).asUInt64();
	t.enter_initialized();
	return t;
}

trackeddatagraphpipelinesession trackeddatagraphpipelinesession_json(const Json::Value& v)
{
	trackeddatagraphpipelinesession t;
	trackable_helper(t, v);
	t.size = (VkDeviceSize)v.get("size", 0).asUInt64();
	t.flags = (VkDataGraphPipelineSessionCreateFlagsARM)v["flags"].asUInt64();
	t.pipeline_index = v["pipeline_index"].asUInt();
	t.tiling = (lava_tiling)v.get("tiling", TILING_LINEAR).asUInt();
	t.object_type = VK_OBJECT_TYPE_DATA_GRAPH_PIPELINE_SESSION_ARM;
	if (v.isMember("memory_flags")) t.memory_flags = (VkMemoryPropertyFlags)v["memory_flags"].asUInt();

	if (v.isMember("parent_device_index")) t.parent_device_index = v["parent_device_index"].asUInt();
	else t.parent_device_index = 0; // use a default for old trace files, and pray we only have one VkDevice

	t.enter_initialized();
	return t;
}

trackedswapchain_replay trackedswapchain_replay_json(const Json::Value& v)
{
	trackedswapchain_replay t;
	trackable_helper(t, v);
	t.info.imageFormat = (VkFormat)v["imageFormat"].asUInt();
	t.info.imageUsage = (VkImageUsageFlags)v["imageUsage"].asUInt();
	t.info.imageExtent.width = v["width"].asUInt();
	t.info.imageExtent.height = v["height"].asUInt();
	t.info.imageSharingMode = (VkSharingMode)v["imageSharingMode"].asUInt();
	t.enter_initialized();
	return t;
}

trackedcmdbuffer trackedcmdbuffer_json(const Json::Value& v)
{
	trackedcmdbuffer t;
	trackable_helper(t, v);
	t.pool_index = v["pool"].asUInt();
	t.enter_initialized();
	t.renderpass_count = v["renderpass_count"].asInt();
	t.shader_command_count = v["shader_command_count"].asInt();
	return t;
}

trackedimageview trackedimageview_json(const Json::Value& v)
{
	trackedimageview t;
	trackable_helper(t, v);
	t.image_index = v["image"].asUInt();
	t.enter_initialized();
	return t;
}

trackedbufferview trackedbufferview_json(const Json::Value& v)
{
	trackedbufferview t;
	trackable_helper(t, v);
	t.buffer_index = v["buffer"].asUInt();
	t.enter_initialized();
	return t;
}

trackeddescriptorset trackeddescriptorset_json(const Json::Value& v)
{
	trackeddescriptorset t;
	trackable_helper(t, v);
	t.pool_index = v["pool"].asUInt();
	t.enter_initialized();
	return t;
}

trackedqueue trackedqueue_json(const Json::Value& v)
{
	trackedqueue t;
	trackable_helper(t, v);
	t.device_index = v["parent_device_index"].asUInt();
	if (v.isMember("queueFamily")) t.queueFamily = v["queueFamily"].asUInt();
	if (v.isMember("queueIndex")) t.queueIndex = v["queueIndex"].asUInt();
	if (v.isMember("queueFlags")) t.queueFlags = (VkQueueFlags)v["queueFlags"].asUInt();
	t.enter_initialized();
	return t;
}

trackeddevice trackeddevice_json(const Json::Value& v)
{
	trackeddevice t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackedphysicaldevice trackedphysicaldevice_json(const Json::Value& v)
{
	trackedphysicaldevice t;
	trackable_helper(t, v);
	if (v.isMember("deviceType")) t.deviceType = (VkPhysicalDeviceType)v["deviceType"].asUInt();
	t.enter_initialized();
	return t;
}

trackedframebuffer trackedframebuffer_json(const Json::Value& v)
{
	trackedframebuffer t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackedshadermodule trackedshadermodule_json(const Json::Value& v)
{
	trackedshadermodule t;
	trackable_helper(t, v);
	if (v.isMember("size")) t.name = v["size"].asInt();
	if (v.isMember("enables_device_address")) t.enables_device_address = v["enables_device_address"].asBool();
	t.enter_initialized();
	return t;
}

trackedrenderpass trackedrenderpass_json(const Json::Value& v)
{
	trackedrenderpass t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackedindirectexecutionset trackedindirectexecutionset_json(const Json::Value& v)
{
	trackedindirectexecutionset t;
	trackable_helper(t, v);
	t.type = (VkIndirectExecutionSetInfoTypeEXT)v["type"].asUInt();
	t.enter_initialized();
	return t;
}

trackedindirectcommandslayout trackedindirectcommandslayout_json(const Json::Value& v)
{
	trackedindirectcommandslayout t;
	trackable_helper(t, v);
	t.flags = (VkIndirectCommandsLayoutUsageFlagsEXT)v["flags"].asUInt();
	t.stages = (VkShaderStageFlags)v["stages"].asUInt();
	t.indirectStride = v["indirect_stride"].asUInt();
	if (v.isMember("pipeline_layout_index")) t.pipeline_layout_index = v["pipeline_layout_index"].asUInt();
	t.enter_initialized();
	return t;
}

trackedpipelinelayout trackedpipelinelayout_json(const Json::Value& v)
{
	trackedpipelinelayout t;
	trackable_helper(t, v);
	if (v.isMember("push_constant_space_used")) t.push_constant_space_used = v["push_constant_space_used"].asUInt();
	if (v.isMember("layout_indices"))
	{
		for (const auto& layout_index : v["layout_indices"])
		{
			t.layout_indices.push_back(layout_index.asUInt());
		}
	}
	t.enter_initialized();
	return t;
}

trackeddescriptorsetlayout trackeddescriptorsetlayout_json(const Json::Value& v)
{
	trackeddescriptorsetlayout t;
	trackable_helper(t, v);
	t.enter_initialized();
	return t;
}

trackeddescriptorupdatetemplate trackeddescriptorupdatetemplate_json(const Json::Value& v)
{
	trackeddescriptorupdatetemplate t;
	trackable_helper(t, v);
	t.type = (VkDescriptorUpdateTemplateType)v["template_type"].asUInt();
	t.flags = v["create_flags"].asUInt();
	t.data_size = v.get("data_size", 0).asUInt64();
	t.enter_initialized();
	return t;
}

trackedshaderobject trackedshaderobject_json(const Json::Value& v)
{
	trackedshaderobject t;
	trackable_helper(t, v);
	t.stage.flags = (VkShaderCreateFlagsEXT)v["flags"].asUInt();
	t.stage.stage = (VkShaderStageFlagBits)v["stage"].asUInt();
	t.stage.name = v["entry_name"].asString();
	return t;
}

trackedsurface trackedsurface_json(const Json::Value& v)
{
	trackedsurface t;
	trackable_helper(t, v);
	t.width = v["width"].asUInt();
	t.height = v["height"].asUInt();
	t.x = v["x"].asInt();
	t.y = v["y"].asInt();
	return t;
}

static const char* marking_type_json_name(VkMarkingTypeARM type)
{
	switch (type)
	{
	case VK_MARKING_TYPE_DEVICE_ADDRESS_ARM: return "device_address";
	case VK_MARKING_TYPE_DESCRIPTOR_SIZE_ARM: return "descriptor_size";
	case VK_MARKING_TYPE_DESCRIPTOR_OFFSET_ARM: return "descriptor_offset";
	case VK_MARKING_TYPE_DESCRIPTOR_ARM: return "descriptor";
	case VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM: return "shader_group_handle";
	default: assert(false); return "invalid";
	}
}

static const char* device_address_type_json_name(VkDeviceAddressTypeARM type)
{
	switch (type)
	{
	case VK_DEVICE_ADDRESS_TYPE_BUFFER_ARM: return "buffer";
	case VK_DEVICE_ADDRESS_TYPE_ACCELERATION_STRUCTURE_ARM: return "acceleration_structure";
	default: assert(false); return "invalid";
	}
}

static const char* descriptor_type_json_name(VkDescriptorType type)
{
	switch (type)
	{
	case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined_image_sampler";
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled_image";
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage_image";
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniform_texel_buffer";
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storage_texel_buffer";
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform_buffer";
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage_buffer";
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform_buffer_dynamic";
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage_buffer_dynamic";
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input_attachment";
	case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: return "inline_uniform_block";
	case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration_structure_khr";
	case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV: return "acceleration_structure_nv";
	case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM: return "sample_weight_image_qcom";
	case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM: return "block_match_image_qcom";
	case VK_DESCRIPTOR_TYPE_TENSOR_ARM: return "tensor_arm";
	case VK_DESCRIPTOR_TYPE_MUTABLE_EXT: return "mutable_ext";
	case VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV: return "partitioned_acceleration_structure_nv";
	default: assert(false); return "invalid";
	}
}

static const char* shader_group_type_json_name(VkShaderGroupShaderKHR type)
{
	switch (type)
	{
	case VK_SHADER_GROUP_SHADER_GENERAL_KHR: return "general";
	case VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR: return "closest_hit";
	case VK_SHADER_GROUP_SHADER_ANY_HIT_KHR: return "any_hit";
	case VK_SHADER_GROUP_SHADER_INTERSECTION_KHR: return "intersection";
	default: assert(false); return "invalid";
	}
}

Json::Value marked_offsets_json(const VkMarkedOffsetsARM* markings)
{
	assert(markings);
	assert(markings->sType == VK_STRUCTURE_TYPE_MARKED_OFFSETS_ARM);

	Json::Value item;
	item["type"] = "VkMarkedOffsetsARM";
	item["sType"] = (Json::Value::UInt64)markings->sType;
	item["count"] = (Json::Value::UInt64)markings->count;

	Json::Value entries(Json::arrayValue);
	if (markings->count > 0)
	{
		assert(markings->pMarkingTypes);
		assert(markings->pSubTypes);
		assert(markings->pOffsets);
	}

	for (uint32_t i = 0; i < markings->count; i++)
	{
		const VkMarkingTypeARM type = markings->pMarkingTypes[i];
		const VkMarkingSubTypeARM subtype = markings->pSubTypes[i];

		Json::Value entry;
		entry["index"] = (Json::Value::UInt64)i;
		entry["offset"] = (Json::Value::UInt64)markings->pOffsets[i];
		entry["type"] = marking_type_json_name(type);

		Json::Value subtype_json;
		switch (type)
		{
		case VK_MARKING_TYPE_DEVICE_ADDRESS_ARM:
			subtype_json["deviceAddressType"] = device_address_type_json_name(subtype.deviceAddressType);
			break;
		case VK_MARKING_TYPE_DESCRIPTOR_SIZE_ARM:
		case VK_MARKING_TYPE_DESCRIPTOR_OFFSET_ARM:
		case VK_MARKING_TYPE_DESCRIPTOR_ARM:
			subtype_json["descriptorType"] = descriptor_type_json_name(subtype.descriptorType);
			break;
		case VK_MARKING_TYPE_SHADER_GROUP_HANDLE_ARM:
			subtype_json["shaderGroupType"] = shader_group_type_json_name(subtype.shaderGroupType);
			break;
		default:
			assert(false);
			break;
		}
		entry["subtype"] = subtype_json;
		entries.append(entry);
	}

	item["markings"] = entries;
	return item;
}

void write_json(FILE* fp, const Json::Value& v)
{
	Json::StyledWriter writer;
	std::string data = writer.write(v);
	size_t written;
	int err = 0;
	do {
		written = fwrite(data.c_str(), data.size(), 1, fp);
		err = ferror(fp);
	} while (!err && !written);
	if (err)
	{
		ELOG("Failed to write dictionary: %s", strerror(err));
	}
}

void write_json(const std::string& path, const Json::Value& v)
{
	FILE* fp = fopen(path.c_str(), "w");
	if (!fp)
	{
		ELOG("Failed to open \"%s\": %s", path.c_str(), strerror(errno));
		return;
	}
	write_json(fp, v);
	fclose(fp);
}

Json::Value from_change_source(const change_source& c)
{
	Json::Value v;
	v["index"] = c.packet;
	v["frame"] = c.frame;
	v["thread"] = c.thread;
	const uint8_t packet = c.packet_type != UINT8_MAX ? c.packet_type : (c.call_id != UINT16_MAX ? PACKET_VULKAN_API_CALL : UINT8_MAX);
	if (packet != UINT8_MAX) v["name"] = get_packet_name((packet_type)packet, c.call_id);
	else v["name"] = "unknown";
	return v;
}
