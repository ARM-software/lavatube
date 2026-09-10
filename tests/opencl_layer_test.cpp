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
	cl_uint layer_entries = 0;
	const cl_icd_dispatch* layer_dispatch = nullptr;
	const cl_uint target_entries = sizeof(target_dispatch) / sizeof(void*);
	assert(init_layer(1, &target_dispatch, &layer_entries, &layer_dispatch) == CL_SUCCESS);
	assert(layer_entries == 1);
	assert(layer_dispatch);
	assert(layer_dispatch->clGetPlatformIDs);
	assert(layer_dispatch->clGetPlatformIDs != fake_clGetPlatformIDs);

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
	assert(access(argv[2], F_OK) != 0);
	assert(deinit_layer() == CL_SUCCESS);
	assert(access(argv[2], F_OK) == 0);

	assert(dlclose(library) == 0);
	return 0;
}
