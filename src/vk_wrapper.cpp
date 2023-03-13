#include <cassert>
#include <stdlib.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

#include "vk_wrapper_auto.h"
#include "util.h"

VkuVulkanLibrary _private_ptr = nullptr;

VkuVulkanLibrary vkuCreateWrapper()
{
	std::string filepath = get_vulkan_lib_path();
	void *library = dlopen(filepath.c_str(), RTLD_NOW | RTLD_LOCAL);

	if (!library)
	{
		ABORT("Failed to load Vulkan library: %s: %s", filepath.c_str(), dlerror());
	}
	else
	{
		ILOG("Successfully loaded Vulkan library: %s", filepath.c_str());
		wrap_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
		if (!wrap_vkGetInstanceProcAddr)
		{
			ABORT("Failed to load vkGetInstanceProcAddr() from the vulkan library: %s", dlerror());
		}

		wrap_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(library, "vkGetDeviceProcAddr"));
		if (!wrap_vkGetDeviceProcAddr)
		{
			ABORT("Failed to load vkGetDeviceProcAddr() from the vulkan library: %s", dlerror());
		}

		wrap_vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(wrap_vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
		assert(wrap_vkEnumerateInstanceExtensionProperties);
		wrap_vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(wrap_vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
		assert(wrap_vkEnumerateInstanceLayerProperties);
		wrap_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(wrap_vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
		assert(wrap_vkCreateInstance);

		// Dont assert on this, we dont care
		wrap_vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(wrap_vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
	}

	_private_ptr = library;
	return library;
}

void vkuDestroyWrapper(VkuVulkanLibrary library)
{
	if (library)
	{
		dlclose(library);
	}
}

void print_extension_mismatch(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo)
{
	uint32_t propertyCount = 0;
	VkResult result = wrap_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &propertyCount, nullptr); // call first to get correct count on host
	(void)result;
	assert(result == VK_SUCCESS);
	std::vector<VkExtensionProperties> tmp_device_extension_properties(propertyCount);
	result = wrap_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &propertyCount, tmp_device_extension_properties.data());
	ILOG("Mismatch between supported and requested device extension sets! Not supported device extension requested:");
	for (unsigned i = 0; i < pCreateInfo->enabledExtensionCount; i++)
	{
		bool found = false;
		for (const auto &ext : tmp_device_extension_properties)
		{
			std::string name = ext.extensionName;
			if (name == pCreateInfo->ppEnabledExtensionNames[i]) found = true;
		}
		if (!found) ILOG("\t%s", pCreateInfo->ppEnabledExtensionNames[i]);
	}
}

void print_feature_mismatch(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo)
{
	VkPhysicalDeviceFeatures device_features = {};
	wrap_vkGetPhysicalDeviceFeatures(physicalDevice, &device_features);
	ILOG("Mismatch between supported and requested feature sets! Mismatched features:");
	if (pCreateInfo->pEnabledFeatures->robustBufferAccess == true && device_features.robustBufferAccess == false) ILOG("\trobustBufferAccess");
	if (pCreateInfo->pEnabledFeatures->fullDrawIndexUint32 == true && device_features.fullDrawIndexUint32 == false) ILOG("\tfullDrawIndexUint32");
	if (pCreateInfo->pEnabledFeatures->imageCubeArray == true && device_features.imageCubeArray == false) ILOG("\timageCubeArray");
	if (pCreateInfo->pEnabledFeatures->independentBlend == true && device_features.independentBlend == false) ILOG("\tindependentBlend");
	if (pCreateInfo->pEnabledFeatures->geometryShader == true && device_features.geometryShader == false) ILOG("\tgeometryShader");
	if (pCreateInfo->pEnabledFeatures->tessellationShader == true && device_features.tessellationShader == false) ILOG("\ttessellationShader");
	if (pCreateInfo->pEnabledFeatures->sampleRateShading == true && device_features.sampleRateShading == false) ILOG("\tsampleRateShading");
	if (pCreateInfo->pEnabledFeatures->dualSrcBlend == true && device_features.dualSrcBlend == false) ILOG("\tdualSrcBlend");
	if (pCreateInfo->pEnabledFeatures->logicOp == true && device_features.logicOp == false) ILOG("\tlogicOp");
	if (pCreateInfo->pEnabledFeatures->multiDrawIndirect == true && device_features.multiDrawIndirect == false) ILOG("\tmultiDrawIndirect");
	if (pCreateInfo->pEnabledFeatures->drawIndirectFirstInstance == true && device_features.drawIndirectFirstInstance == false) ILOG("\tdrawIndirectFirstInstance");
	if (pCreateInfo->pEnabledFeatures->depthClamp == true && device_features.depthClamp == false) ILOG("\tdepthClamp");
	if (pCreateInfo->pEnabledFeatures->depthBiasClamp == true && device_features.depthBiasClamp == false) ILOG("\tdepthBiasClamp");
	if (pCreateInfo->pEnabledFeatures->fillModeNonSolid == true && device_features.fillModeNonSolid == false) ILOG("\tfillModeNonSolid");
	if (pCreateInfo->pEnabledFeatures->depthBounds == true && device_features.depthBounds == false) ILOG("\tdepthBounds");
	if (pCreateInfo->pEnabledFeatures->wideLines == true && device_features.wideLines == false) ILOG("\twideLines");
	if (pCreateInfo->pEnabledFeatures->largePoints == true && device_features.largePoints == false) ILOG("\tlargePoints");
	if (pCreateInfo->pEnabledFeatures->alphaToOne == true && device_features.alphaToOne == false) ILOG("\talphaToOne");
	if (pCreateInfo->pEnabledFeatures->multiViewport == true && device_features.multiViewport == false) ILOG("\tmultiViewport");
	if (pCreateInfo->pEnabledFeatures->samplerAnisotropy == true && device_features.samplerAnisotropy == false) ILOG("\tsamplerAnisotropy");
	if (pCreateInfo->pEnabledFeatures->textureCompressionETC2 == true && device_features.textureCompressionETC2 == false) ILOG("\ttextureCompressionETC2");
	if (pCreateInfo->pEnabledFeatures->textureCompressionASTC_LDR == true && device_features.textureCompressionASTC_LDR == false) ILOG("\ttextureCompressionASTC_LDR");
	if (pCreateInfo->pEnabledFeatures->textureCompressionBC == true && device_features.textureCompressionBC == false) ILOG("\ttextureCompressionBC");
	if (pCreateInfo->pEnabledFeatures->occlusionQueryPrecise == true && device_features.occlusionQueryPrecise == false) ILOG("\tocclusionQueryPrecise");
	if (pCreateInfo->pEnabledFeatures->pipelineStatisticsQuery == true && device_features.pipelineStatisticsQuery == false) ILOG("\tpipelineStatisticsQuery");
	if (pCreateInfo->pEnabledFeatures->vertexPipelineStoresAndAtomics == true && device_features.vertexPipelineStoresAndAtomics == false) ILOG("\tvertexPipelineStoresAndAtomics");
	if (pCreateInfo->pEnabledFeatures->fragmentStoresAndAtomics == true && device_features.fragmentStoresAndAtomics == false) ILOG("\tfragmentStoresAndAtomics");
	if (pCreateInfo->pEnabledFeatures->shaderTessellationAndGeometryPointSize == true && device_features.shaderTessellationAndGeometryPointSize == false) ILOG("\tshaderTessellationAndGeometryPointSize");
	if (pCreateInfo->pEnabledFeatures->shaderImageGatherExtended == true && device_features.shaderImageGatherExtended == false) ILOG("\tshaderImageGatherExtended");
	if (pCreateInfo->pEnabledFeatures->shaderStorageImageExtendedFormats == true && device_features.shaderStorageImageExtendedFormats == false) ILOG("\tshaderStorageImageExtendedFormats");
	if (pCreateInfo->pEnabledFeatures->shaderStorageImageMultisample == true && device_features.shaderStorageImageMultisample == false) ILOG("\tshaderStorageImageMultisample");
	if (pCreateInfo->pEnabledFeatures->shaderStorageImageReadWithoutFormat == true && device_features.shaderStorageImageReadWithoutFormat == false) ILOG("\tshaderStorageImageReadWithoutFormat");
	if (pCreateInfo->pEnabledFeatures->shaderStorageImageWriteWithoutFormat == true && device_features.shaderStorageImageWriteWithoutFormat == false) ILOG("\tshaderStorageImageWriteWithoutFormat");
	if (pCreateInfo->pEnabledFeatures->shaderUniformBufferArrayDynamicIndexing == true && device_features.shaderUniformBufferArrayDynamicIndexing == false) ILOG("\tshaderUniformBufferArrayDynamicIndexing");
	if (pCreateInfo->pEnabledFeatures->shaderSampledImageArrayDynamicIndexing == true && device_features.shaderSampledImageArrayDynamicIndexing == false) ILOG("\tshaderSampledImageArrayDynamicIndexing");
	if (pCreateInfo->pEnabledFeatures->shaderStorageBufferArrayDynamicIndexing == true && device_features.shaderStorageBufferArrayDynamicIndexing == false) ILOG("\tshaderStorageBufferArrayDynamicIndexing");
	if (pCreateInfo->pEnabledFeatures->shaderStorageImageArrayDynamicIndexing == true && device_features.shaderStorageImageArrayDynamicIndexing == false) ILOG("\tshaderStorageImageArrayDynamicIndexing");
	if (pCreateInfo->pEnabledFeatures->shaderClipDistance == true && device_features.shaderClipDistance == false) ILOG("\tshaderClipDistance");
	if (pCreateInfo->pEnabledFeatures->shaderCullDistance == true && device_features.shaderCullDistance == false) ILOG("\tshaderCullDistance");
	if (pCreateInfo->pEnabledFeatures->shaderFloat64 == true && device_features.shaderFloat64 == false) ILOG("\tshaderFloat64");
	if (pCreateInfo->pEnabledFeatures->shaderInt64 == true && device_features.shaderInt64 == false) ILOG("\tshaderInt64");
	if (pCreateInfo->pEnabledFeatures->shaderInt16 == true && device_features.shaderInt16 == false) ILOG("\tshaderInt16");
	if (pCreateInfo->pEnabledFeatures->shaderResourceResidency == true && device_features.shaderResourceResidency == false) ILOG("\tshaderResourceResidency");
	if (pCreateInfo->pEnabledFeatures->shaderResourceMinLod == true && device_features.shaderResourceMinLod == false) ILOG("\tshaderResourceMinLod");
	if (pCreateInfo->pEnabledFeatures->sparseBinding == true && device_features.sparseBinding == false) ILOG("\tsparseBinding");
	if (pCreateInfo->pEnabledFeatures->sparseResidencyBuffer == true && device_features.sparseResidencyBuffer == false) ILOG("\tsparseResidencyBuffer");
	if (pCreateInfo->pEnabledFeatures->sparseResidencyImage2D == true && device_features.sparseResidencyImage2D == false) ILOG("\tsparseResidencyImage2D");
	if (pCreateInfo->pEnabledFeatures->sparseResidencyImage3D == true && device_features.sparseResidencyImage3D == false) ILOG("\tsparseResidencyImage3D");
	if (pCreateInfo->pEnabledFeatures->sparseResidency2Samples == true && device_features.sparseResidency2Samples == false) ILOG("\tsparseResidency2Samples");
	if (pCreateInfo->pEnabledFeatures->sparseResidency4Samples == true && device_features.sparseResidency4Samples == false) ILOG("\tsparseResidency4Samples");
	if (pCreateInfo->pEnabledFeatures->sparseResidency8Samples == true && device_features.sparseResidency8Samples == false) ILOG("\tsparseResidency8Samples");
	if (pCreateInfo->pEnabledFeatures->sparseResidency16Samples == true && device_features.sparseResidency16Samples == false) ILOG("\tsparseResidency16Samples");
	if (pCreateInfo->pEnabledFeatures->sparseResidencyAliased == true && device_features.sparseResidencyAliased == false) ILOG("\tsparseResidencyAliased");
	if (pCreateInfo->pEnabledFeatures->variableMultisampleRate == true && device_features.variableMultisampleRate == false) ILOG("\tvariableMultisampleRate");
	if (pCreateInfo->pEnabledFeatures->inheritedQueries == true && device_features.inheritedQueries == false) ILOG("\tinheritedQueries");
	ABORT("Mismatched feature sets!");
}
