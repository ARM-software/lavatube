// --- JSON write helpers ---
// We only write out static info that does not change for the duration of the trace.

#include "jsoncpp/json/writer.h"

#include "json_helpers.h"

Json::Value trackable_json(const trackable* t)
{
	Json::Value v;
	assert(t->creation.frame != UINT32_MAX);
	assert(t->last_modified.frame != UINT32_MAX);
	v["frame_created"] = t->creation.frame;
	v["call_created"] = t->creation.call; // local call number
	v["thread_created"] = t->creation.thread;
	v["api_created"] = t->creation.call_id;
	if (t->destroyed.frame != UINT32_MAX)
	{
		v["frame_destroyed"] = t->destroyed.frame;
		v["call_destroyed"] = t->destroyed.call;
		v["thread_destroyed"] = t->destroyed.thread;
		v["api_destroyed"] = t->destroyed.call_id;
	}
	if (!t->name.empty()) v["name"] = t->name;
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
	v["req_size"] = (Json::Value::UInt64)t->req.size; // info only, do not read
	v["req_alignment"] = (unsigned)t->req.alignment; // info only, do not read
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
	for (unsigned i = 0; i < t->strides.size() ; i++)
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

Json::Value trackedcmdbuffer_trace_json(const trackedcmdbuffer_trace* t)
{
	Json::Value v = trackable_json(t);
	v["pool"] = (unsigned)t->pool_index;
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

Json::Value trackedpipelinelayout_json(const trackedpipelinelayout* t)
{
	Json::Value v = trackable_json(t);
	v["push_constant_space_used"] = t->push_constant_space_used;
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

// --- JSON read helpers ---

static void trackable_helper(trackable& t, const Json::Value& v)
{
	t.index = v["index"].asUInt();
	t.creation.frame = v["frame_created"].asUInt();
	if (v.isMember("call_created")) t.creation.call = v["call_created"].asUInt();
	if (v.isMember("thread_created")) t.creation.thread = v["thread_created"].asUInt();
	if (v.isMember("api_created")) t.creation.call_id = v["api_created"].asUInt();
	if (v.isMember("frame_destroyed")) // check for legacy value of -1
	{
		if (v["frame_destroyed"].type() == Json::intValue && v["frame_destroyed"].asInt() == -1) { t.destroyed.frame = UINT32_MAX; }
		else t.destroyed.frame = v["frame_destroyed"].asUInt();
	}
	if (v.isMember("call_destroyed")) t.destroyed.call = v["call_destroyed"].asUInt();
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
	t.object_type = VK_OBJECT_TYPE_BUFFER;
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

trackedimage trackedimage_json(const Json::Value& v)
{
	trackedimage t;
	trackable_helper(t, v);
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
	t.flags = (VkTensorCreateFlagsARM)v["flags"].asUInt64();
	t.sharingMode = (VkSharingMode)v["sharingMode"].asUInt();
	t.object_type = VK_OBJECT_TYPE_TENSOR_ARM;
	t.tiling = (lava_tiling)v["tiling"].asUInt();
	t.format = (VkFormat)v["format"].asUInt();
	t.usage = (VkTensorUsageFlagsARM)v["dimensions"].asUInt64();
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

trackedpipelinelayout trackedpipelinelayout_json(const Json::Value& v)
{
	trackedpipelinelayout t;
	trackable_helper(t, v);
	if (v.isMember("push_constant_space_used")) t.push_constant_space_used = v["push_constant_space_used"].asUInt();
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
