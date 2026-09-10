#include <algorithm>
#include <cstring>

#include "write.h"

static cl_int CL_API_CALL trace_clGetPlatformIDs(cl_uint num_entries, cl_platform_id* platforms, cl_uint* num_platforms)
{
	lava_writer& instance = lava_writer::instance();
	opencl_layer_runtime_state& state = instance.opencl_layer;
	if (!state.initialized.load(std::memory_order_acquire) || !state.target_dispatch.clGetPlatformIDs)
	{
		ABORT("clGetPlatformIDs called through an uninitialized OpenCL layer");
	}

	instance.ensure_started(get_trace_path("opencl"));
	lava_file_writer& writer = instance.file_writer();
	writer.begin_packet(PACKET_OPENCL_API_CALL);
	writer.write_uint16_t(OPENCL_DICTIONARY_BASE + OPENCL_clGetPlatformIDs);
	writer.write_uint32_t(0);
	writer.current.call_id = OPENCL_clGetPlatformIDs;
	writer.current.frame = instance.global_frame.load(std::memory_order_relaxed);

	const uint_fast64_t call = state.platform_id_calls.fetch_add(1, std::memory_order_relaxed) + 1;
	DLOG("OpenCL layer intercepted clGetPlatformIDs call %lu", (unsigned long)call);
	const cl_int result = state.target_dispatch.clGetPlatformIDs(num_entries, platforms, num_platforms);

	const cl_uint returned_num_platforms = result == CL_SUCCESS && num_platforms ? *num_platforms : 0;
	if (result == CL_SUCCESS && num_platforms)
	{
		state.known_platform_count.store(returned_num_platforms, std::memory_order_relaxed);
	}
	cl_uint platform_count = 0;
	if (result == CL_SUCCESS && platforms && num_entries)
	{
		cl_uint available = returned_num_platforms;
		if (!num_platforms)
		{
			available = state.known_platform_count.load(std::memory_order_relaxed);
			if (available == UINT32_MAX)
			{
				cl_uint queried_platform_count = 0;
				const cl_int query_result = state.target_dispatch.clGetPlatformIDs(
					0, nullptr, &queried_platform_count);
				available = query_result == CL_SUCCESS ? queried_platform_count : num_entries;
				if (query_result == CL_SUCCESS)
				{
					state.known_platform_count.store(available, std::memory_order_relaxed);
				}
			}
		}
		platform_count = std::min(num_entries, available);
	}
	std::vector<uint32_t> platform_indices(platform_count, CONTAINER_NULL_VALUE);
	if (platform_count)
	{
		lava::lock_guard lock(frame_mutex);
		for (cl_uint i = 0; i < platform_count; i++)
		{
			if (!platforms[i]) continue;
			trackable* data = nullptr;
			if (state.platform_index.contains(platforms[i]))
			{
				data = state.platform_index.at(platforms[i]);
			}
			else
			{
				data = state.platform_index.add(platforms[i], writer.current);
				data->enter_created();
			}
			platform_indices[i] = data->index;
		}
	}

	uint8_t pointer_flags = 0;
	if (platforms) pointer_flags |= 1;
	if (num_platforms) pointer_flags |= 2;
	writer.write_uint32_t(num_entries);
	writer.write_uint8_t(pointer_flags);
	writer.write_uint32_t(returned_num_platforms);
	writer.write_uint32_t((uint32_t)platform_indices.size());
	writer.write_array(platform_indices.data(), platform_indices.size());
	writer.write_int32_t(result);
	writer.end_packet();
	return result;
}

static cl_int initialize_opencl_layer(cl_uint num_entries, const cl_icd_dispatch* target_dispatch,
	cl_uint* num_entries_ret, const cl_icd_dispatch** layer_dispatch_ret)
{
	if (!target_dispatch || !num_entries_ret || !layer_dispatch_ret || num_entries == 0
	    || !target_dispatch->clGetPlatformIDs)
	{
		return CL_INVALID_VALUE;
	}

	lava_writer& writer = lava_writer::instance();
	opencl_layer_runtime_state& state = writer.opencl_layer;
	lava::lock_guard lock(frame_mutex);

	const cl_uint state_entries = sizeof(state.layer_dispatch) / sizeof(void*);
	const cl_uint copy_entries = std::min(num_entries, state_entries);
	memset(&state.target_dispatch, 0, sizeof(state.target_dispatch));
	memset(&state.layer_dispatch, 0, sizeof(state.layer_dispatch));
	memcpy(&state.target_dispatch, target_dispatch, copy_entries * sizeof(void*));
	memcpy(&state.layer_dispatch, target_dispatch, copy_entries * sizeof(void*));
	state.layer_dispatch.clGetPlatformIDs = trace_clGetPlatformIDs;
	state.known_platform_count.store(UINT32_MAX, std::memory_order_relaxed);
	state.initialized.store(true, std::memory_order_release);

	*num_entries_ret = copy_entries;
	*layer_dispatch_ret = &state.layer_dispatch;
	DLOG("Initialized OpenCL layer with %u dispatch entries using writer %p", copy_entries, (void*)&writer);
	return CL_SUCCESS;
}

extern "C" EXPORT CL_API_ENTRY cl_int CL_API_CALL clGetLayerInfo(cl_layer_info param_name,
	size_t param_value_size, void* param_value, size_t* param_value_size_ret)
{
	if (param_name == CL_LAYER_API_VERSION)
	{
		if (param_value && param_value_size < sizeof(cl_layer_api_version)) return CL_INVALID_VALUE;
		if (param_value) *(cl_layer_api_version*)param_value = CL_LAYER_API_VERSION_100;
		if (param_value_size_ret) *param_value_size_ret = sizeof(cl_layer_api_version);
		return CL_SUCCESS;
	}
	if (param_name == CL_LAYER_NAME)
	{
		const char layer_name[] = "Lavatube OpenCL capture layer";
		if (param_value && param_value_size < sizeof(layer_name)) return CL_INVALID_VALUE;
		if (param_value) memcpy(param_value, layer_name, sizeof(layer_name));
		if (param_value_size_ret) *param_value_size_ret = sizeof(layer_name);
		return CL_SUCCESS;
	}
	return CL_INVALID_VALUE;
}

extern "C" EXPORT CL_API_ENTRY cl_int CL_API_CALL clInitLayer(cl_uint num_entries,
	const cl_icd_dispatch* target_dispatch, cl_uint* num_entries_ret,
	const cl_icd_dispatch** layer_dispatch_ret)
{
	return initialize_opencl_layer(num_entries, target_dispatch, num_entries_ret, layer_dispatch_ret);
}

extern "C" EXPORT CL_API_ENTRY cl_int CL_API_CALL clInitLayerWithProperties(cl_uint num_entries,
	const cl_icd_dispatch* target_dispatch, cl_uint* num_entries_ret,
	const cl_icd_dispatch** layer_dispatch_ret, const cl_layer_properties* properties)
{
	if (properties && properties[0] != CL_LAYER_PROPERTIES_LIST_END) return CL_INVALID_PROPERTY;
	return initialize_opencl_layer(num_entries, target_dispatch, num_entries_ret, layer_dispatch_ret);
}

extern "C" EXPORT CL_API_ENTRY cl_int CL_API_CALL clDeinitLayer(void)
{
	lava_writer& writer = lava_writer::instance();
	{
		lava::lock_guard lock(frame_mutex);
		opencl_layer_runtime_state& state = writer.opencl_layer;
		DLOG("Deinitialized OpenCL layer after %lu clGetPlatformIDs calls",
		     (unsigned long)state.platform_id_calls.load(std::memory_order_relaxed));
		state.initialized.store(false, std::memory_order_release);
	}
	trace_finalize_if_inactive();
	return CL_SUCCESS;
}
