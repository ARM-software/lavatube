#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <CL/cl_layer.h>

static cl_int CL_API_CALL fake_clGetPlatformIDs(cl_uint num_entries, cl_platform_id* platforms,
	cl_uint* num_platforms)
{
	if (num_platforms) *num_platforms = 2;
	if (platforms && num_entries >= 2)
	{
		platforms[0] = reinterpret_cast<cl_platform_id>(0x1000);
		platforms[1] = reinterpret_cast<cl_platform_id>(0x2000);
	}
	return CL_SUCCESS;
}

static cl_int CL_API_CALL fake_clGetDeviceIDs(cl_platform_id platform, cl_device_type device_type,
	cl_uint num_entries, cl_device_id* devices, cl_uint* num_devices)
{
	(void)device_type;
	if (platform != reinterpret_cast<cl_platform_id>(0x1000)) return CL_INVALID_PLATFORM;
	if (num_devices) *num_devices = 2;
	if (devices && num_entries >= 2)
	{
		devices[0] = reinterpret_cast<cl_device_id>(0x3000);
		devices[1] = reinterpret_cast<cl_device_id>(0x4000);
	}
	return CL_SUCCESS;
}

int main(int argc, char** argv)
{
	assert(argc == 3);
	assert(setenv("LAVATUBE_DESTINATION", argv[2], 1) == 0);
	(void)unlink(argv[2]);
	void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!library)
	{
		fprintf(stderr, "Failed to load OpenCL layer: %s\n", dlerror());
		return 1;
	}

	clGetLayerInfo_fn get_layer_info = (clGetLayerInfo_fn)dlsym(library, "clGetLayerInfo");
	clInitLayer_fn init_layer = (clInitLayer_fn)dlsym(library, "clInitLayer");
	clInitLayerWithProperties_fn init_layer_with_properties =
		(clInitLayerWithProperties_fn)dlsym(library, "clInitLayerWithProperties");
	clDeinitLayer_fn deinit_layer = (clDeinitLayer_fn)dlsym(library, "clDeinitLayer");
	assert(get_layer_info);
	assert(init_layer);
	assert(init_layer_with_properties);
	assert(deinit_layer);

	cl_layer_api_version version = 0;
	size_t value_size = 0;
	assert(get_layer_info(CL_LAYER_API_VERSION, sizeof(version), &version, &value_size) == CL_SUCCESS);
	assert(version == CL_LAYER_API_VERSION_100);
	assert(value_size == sizeof(version));
	assert(get_layer_info(CL_LAYER_API_VERSION, sizeof(version) - 1, &version, nullptr) == CL_INVALID_VALUE);

	char name[64] = {};
	assert(get_layer_info(CL_LAYER_NAME, sizeof(name), name, &value_size) == CL_SUCCESS);
	assert(strcmp(name, "Lavatube OpenCL capture layer") == 0);
	assert(value_size == strlen(name) + 1);

	cl_icd_dispatch target_dispatch = {};
	target_dispatch.clGetPlatformIDs = fake_clGetPlatformIDs;
	target_dispatch.clGetDeviceIDs = fake_clGetDeviceIDs;
	cl_uint layer_entries = 0;
	const cl_icd_dispatch* layer_dispatch = nullptr;
	const cl_uint target_entries = sizeof(target_dispatch) / sizeof(void*);
	assert(init_layer(1, &target_dispatch, &layer_entries, &layer_dispatch) == CL_SUCCESS);
	assert(layer_entries == 1);
	assert(layer_dispatch);
	assert(layer_dispatch->clGetPlatformIDs);
	assert(layer_dispatch->clGetPlatformIDs != fake_clGetPlatformIDs);
	assert(!layer_dispatch->clGetDeviceIDs);

	cl_uint num_platforms = 0;
	assert(layer_dispatch->clGetPlatformIDs(0, nullptr, &num_platforms) == CL_SUCCESS);
	assert(num_platforms == 2);
	cl_platform_id platforms[2] = {};
	assert(layer_dispatch->clGetPlatformIDs(2, platforms, nullptr) == CL_SUCCESS);
	assert(platforms[0] == reinterpret_cast<cl_platform_id>(0x1000));
	assert(platforms[1] == reinterpret_cast<cl_platform_id>(0x2000));

	const cl_layer_properties properties[] = { CL_LAYER_PROPERTIES_LIST_END };
	const cl_layer_properties unsupported_properties[] = { 1, 0,
		CL_LAYER_PROPERTIES_LIST_END };
	assert(init_layer_with_properties(target_entries, &target_dispatch, &layer_entries,
	                                 &layer_dispatch, unsupported_properties) == CL_INVALID_PROPERTY);
	assert(init_layer_with_properties(target_entries, &target_dispatch, &layer_entries,
	                                 &layer_dispatch, properties) == CL_SUCCESS);
	platforms[0] = nullptr;
	platforms[1] = nullptr;
	assert(layer_dispatch->clGetPlatformIDs(2, platforms, nullptr) == CL_SUCCESS);
	assert(platforms[0] == reinterpret_cast<cl_platform_id>(0x1000));
	assert(platforms[1] == reinterpret_cast<cl_platform_id>(0x2000));
	assert(layer_dispatch->clGetDeviceIDs);
	assert(layer_dispatch->clGetDeviceIDs != fake_clGetDeviceIDs);
	cl_uint num_devices = 0;
	assert(layer_dispatch->clGetDeviceIDs(
		platforms[0], CL_DEVICE_TYPE_ALL, 0, nullptr, &num_devices) == CL_SUCCESS);
	assert(num_devices == 2);
	cl_device_id devices[2] = {};
	assert(layer_dispatch->clGetDeviceIDs(
		platforms[0], CL_DEVICE_TYPE_ALL, 2, devices, nullptr) == CL_SUCCESS);
	assert(devices[0] == reinterpret_cast<cl_device_id>(0x3000));
	assert(devices[1] == reinterpret_cast<cl_device_id>(0x4000));
	devices[0] = nullptr;
	devices[1] = nullptr;
	assert(layer_dispatch->clGetDeviceIDs(
		platforms[0], CL_DEVICE_TYPE_GPU, 2, devices, nullptr) == CL_SUCCESS);
	assert(devices[0] == reinterpret_cast<cl_device_id>(0x3000));
	assert(devices[1] == reinterpret_cast<cl_device_id>(0x4000));
	cl_uint invalid_num_devices = 17;
	assert(layer_dispatch->clGetDeviceIDs(reinterpret_cast<cl_platform_id>(0xdead),
		CL_DEVICE_TYPE_ALL, 0, nullptr, &invalid_num_devices) == CL_INVALID_PLATFORM);
	assert(invalid_num_devices == 17);
	assert(access(argv[2], F_OK) != 0);
	assert(deinit_layer() == CL_SUCCESS);
	assert(access(argv[2], F_OK) == 0);

	assert(dlclose(library) == 0);
	return 0;
}
