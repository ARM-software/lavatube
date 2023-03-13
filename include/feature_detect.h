#pragma once

#include <cassert>
#include <atomic>
#include <vulkan/vulkan.h>

// Handle actually-used feature detection for many features during tracing. Using bool atomics here
// to make the code safe for multi-thread use. We could also store one copy of the feature lists for
// each thread and then combine them after, but this way seems simpler.
//
// We need to do this work because some developers are lazy and just pass the feature structures
// back to the driver as they received it, instead of turning on only the features they actually will
// use, making traces more non-portable than they need to be.
//
// Note that the use of extension-specific entry points is deliberately not checked here, as these
// are checked through the use of the extension mechanism instead.

struct atomicPhysicalDeviceFeatures
{
	std::atomic_bool robustBufferAccess { false };
	std::atomic_bool fullDrawIndexUint32 { false };
	std::atomic_bool imageCubeArray { false };
	std::atomic_bool independentBlend { false };
	std::atomic_bool geometryShader { false };
	std::atomic_bool tessellationShader { false };
	std::atomic_bool sampleRateShading { false };
	std::atomic_bool dualSrcBlend { false };
	std::atomic_bool logicOp { false };
	std::atomic_bool multiDrawIndirect { false };
	std::atomic_bool drawIndirectFirstInstance { false };
	std::atomic_bool depthClamp { false };
	std::atomic_bool depthBiasClamp { false };
	std::atomic_bool fillModeNonSolid { false };
	std::atomic_bool depthBounds { false };
	std::atomic_bool wideLines { false };
	std::atomic_bool largePoints { false };
	std::atomic_bool alphaToOne { false };
	std::atomic_bool multiViewport { false };
	std::atomic_bool samplerAnisotropy { false };
	std::atomic_bool textureCompressionETC2 { false };
	std::atomic_bool textureCompressionASTC_LDR { false };
	std::atomic_bool textureCompressionBC { false };
	std::atomic_bool occlusionQueryPrecise { false };
	std::atomic_bool pipelineStatisticsQuery { false };
	std::atomic_bool vertexPipelineStoresAndAtomics { false };
	std::atomic_bool fragmentStoresAndAtomics { false };
	std::atomic_bool shaderTessellationAndGeometryPointSize { false };
	std::atomic_bool shaderImageGatherExtended { false };
	std::atomic_bool shaderStorageImageExtendedFormats { false };
	std::atomic_bool shaderStorageImageMultisample { false };
	std::atomic_bool shaderStorageImageReadWithoutFormat { false };
	std::atomic_bool shaderStorageImageWriteWithoutFormat { false };
	std::atomic_bool shaderUniformBufferArrayDynamicIndexing { false };
	std::atomic_bool shaderSampledImageArrayDynamicIndexing { false };
	std::atomic_bool shaderStorageBufferArrayDynamicIndexing { false };
	std::atomic_bool shaderStorageImageArrayDynamicIndexing { false };
	std::atomic_bool shaderClipDistance { false };
	std::atomic_bool shaderCullDistance { false };
	std::atomic_bool shaderFloat64 { false };
	std::atomic_bool shaderInt64 { false };
	std::atomic_bool shaderInt16 { false };
	std::atomic_bool shaderResourceResidency { false };
	std::atomic_bool shaderResourceMinLod { false };
	std::atomic_bool sparseBinding { false };
	std::atomic_bool sparseResidencyBuffer { false };
	std::atomic_bool sparseResidencyImage2D { false };
	std::atomic_bool sparseResidencyImage3D { false };
	std::atomic_bool sparseResidency2Samples { false };
	std::atomic_bool sparseResidency4Samples { false };
	std::atomic_bool sparseResidency8Samples { false };
	std::atomic_bool sparseResidency16Samples { false };
	std::atomic_bool sparseResidencyAliased { false };
	std::atomic_bool variableMultisampleRate { false };
	std::atomic_bool inheritedQueries { false };
};

struct atomicPhysicalDeviceVulkan11Features
{
	std::atomic_bool storageBuffer16BitAccess { false };
	std::atomic_bool uniformAndStorageBuffer16BitAccess { false };
	std::atomic_bool storagePushConstant16 { false };
	std::atomic_bool storageInputOutput16 { false };
	std::atomic_bool multiview { false };
	std::atomic_bool multiviewGeometryShader { false };
	std::atomic_bool multiviewTessellationShader { false };
	std::atomic_bool variablePointersStorageBuffer { false };
	std::atomic_bool variablePointers { false };
	std::atomic_bool protectedMemory { false };
	std::atomic_bool samplerYcbcrConversion { false };
	std::atomic_bool shaderDrawParameters { false };
};

struct atomicPhysicalDeviceVulkan12Features
{
	std::atomic_bool samplerMirrorClampToEdge { false };
	std::atomic_bool drawIndirectCount { false };
	std::atomic_bool storageBuffer8BitAccess { false };
	std::atomic_bool uniformAndStorageBuffer8BitAccess { false };
	std::atomic_bool storagePushConstant8 { false };
	std::atomic_bool shaderBufferInt64Atomics { false };
	std::atomic_bool shaderSharedInt64Atomics { false };
	std::atomic_bool shaderFloat16 { false };
	std::atomic_bool shaderInt8 { false };
	std::atomic_bool descriptorIndexing { false };
	std::atomic_bool shaderInputAttachmentArrayDynamicIndexing { false };
	std::atomic_bool shaderUniformTexelBufferArrayDynamicIndexing { false };
	std::atomic_bool shaderStorageTexelBufferArrayDynamicIndexing { false };
	std::atomic_bool shaderUniformBufferArrayNonUniformIndexing { false };
	std::atomic_bool shaderSampledImageArrayNonUniformIndexing { false };
	std::atomic_bool shaderStorageBufferArrayNonUniformIndexing { false };
	std::atomic_bool shaderStorageImageArrayNonUniformIndexing { false };
	std::atomic_bool shaderInputAttachmentArrayNonUniformIndexing { false };
	std::atomic_bool shaderUniformTexelBufferArrayNonUniformIndexing { false };
	std::atomic_bool shaderStorageTexelBufferArrayNonUniformIndexing { false };
	std::atomic_bool descriptorBindingUniformBufferUpdateAfterBind { false };
	std::atomic_bool descriptorBindingSampledImageUpdateAfterBind { false };
	std::atomic_bool descriptorBindingStorageImageUpdateAfterBind { false };
	std::atomic_bool descriptorBindingStorageBufferUpdateAfterBind { false };
	std::atomic_bool descriptorBindingUniformTexelBufferUpdateAfterBind { false };
	std::atomic_bool descriptorBindingStorageTexelBufferUpdateAfterBind { false };
	std::atomic_bool descriptorBindingUpdateUnusedWhilePending { false };
	std::atomic_bool descriptorBindingPartiallyBound { false };
	std::atomic_bool descriptorBindingVariableDescriptorCount { false };
	std::atomic_bool runtimeDescriptorArray { false };
	std::atomic_bool samplerFilterMinmax { false };
	std::atomic_bool scalarBlockLayout { false };
	std::atomic_bool imagelessFramebuffer { false };
	std::atomic_bool uniformBufferStandardLayout { false };
	std::atomic_bool shaderSubgroupExtendedTypes { false };
	std::atomic_bool separateDepthStencilLayouts { false };
	std::atomic_bool hostQueryReset { false };
	std::atomic_bool timelineSemaphore { false };
	std::atomic_bool bufferDeviceAddress { false };
	std::atomic_bool bufferDeviceAddressCaptureReplay { false };
	std::atomic_bool bufferDeviceAddressMultiDevice { false };
	std::atomic_bool vulkanMemoryModel { false };
	std::atomic_bool vulkanMemoryModelDeviceScope { false };
	std::atomic_bool vulkanMemoryModelAvailabilityVisibilityChains { false };
	std::atomic_bool shaderOutputViewportIndex { false };
	std::atomic_bool shaderOutputLayer { false };
	std::atomic_bool subgroupBroadcastDynamicId { false };
};

struct atomicPhysicalDeviceVulkan13Features
{
	std::atomic_bool robustImageAccess { false };
	std::atomic_bool inlineUniformBlock { false };
	std::atomic_bool descriptorBindingInlineUniformBlockUpdateAfterBind { false };
	std::atomic_bool pipelineCreationCacheControl { false };
	std::atomic_bool privateData { false };
	std::atomic_bool shaderDemoteToHelperInvocation { false };
	std::atomic_bool shaderTerminateInvocation { false };
	std::atomic_bool subgroupSizeControl { false };
	std::atomic_bool computeFullSubgroups { false };
	std::atomic_bool synchronization2 { false };
	std::atomic_bool textureCompressionASTC_HDR { false };
	std::atomic_bool shaderZeroInitializeWorkgroupMemory { false };
	std::atomic_bool dynamicRendering { false };
	std::atomic_bool shaderIntegerDotProduct { false };
	std::atomic_bool maintenance4 { false };
};

struct feature_detection
{
private:
	// Tracking data
	struct atomicPhysicalDeviceFeatures core10;
	struct atomicPhysicalDeviceVulkan11Features core11;
	struct atomicPhysicalDeviceVulkan12Features core12;
	struct atomicPhysicalDeviceVulkan13Features core13;

public:
	// --- Checking structures Call these for all these structures after they are successfully used. ---

	void check_VkPipelineShaderStageCreateInfo(const VkPipelineShaderStageCreateInfo* info)
	{
		if (info->stage == VK_SHADER_STAGE_GEOMETRY_BIT) core10.geometryShader = true;
		else if (info->stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT || info->stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) core10.tessellationShader = true;
	}

	void check_VkPipelineColorBlendAttachmentState(const VkPipelineColorBlendAttachmentState* info)
	{
		const VkBlendFactor factors[4] = { VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA };
		for (int i = 0; i < 4; i++) if (info->srcColorBlendFactor == factors[i]) core10.dualSrcBlend = true;
		for (int i = 0; i < 4; i++) if (info->dstColorBlendFactor == factors[i]) core10.dualSrcBlend = true;
		for (int i = 0; i < 4; i++) if (info->srcAlphaBlendFactor == factors[i]) core10.dualSrcBlend = true;
		for (int i = 0; i < 4; i++) if (info->dstAlphaBlendFactor == factors[i]) core10.dualSrcBlend = true;
	}

	void check_VkSamplerCreateInfo(const VkSamplerCreateInfo* info)
	{
		if (info->anisotropyEnable == VK_TRUE) core10.samplerAnisotropy = true;
		if (info->addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
		    || info->addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
		    || info->addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE)
			core12.samplerMirrorClampToEdge = true;
	}

	void check_VkQueryPoolCreateInfo(const VkQueryPoolCreateInfo* info)
	{
		if (info->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS && info->pipelineStatistics != 0) core10.pipelineStatisticsQuery = true;
	}

	void check_VkPipelineColorBlendStateCreateInfo(const VkPipelineColorBlendStateCreateInfo* info)
	{
		if (info->logicOpEnable == VK_TRUE) core10.logicOp = true;
	}

	void check_VkPipelineMultisampleStateCreateInfo(const VkPipelineMultisampleStateCreateInfo* info)
	{
		if (info->alphaToOneEnable == VK_TRUE) core10.alphaToOne = true;
		if (info->sampleShadingEnable == VK_TRUE) core10.sampleRateShading = true;
	}

	void check_VkImageCreateInfo(const VkImageCreateInfo* info)
	{
		if (info->imageType == VK_IMAGE_TYPE_2D && info->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)
		{
			if (info->samples == VK_SAMPLE_COUNT_1_BIT) core10.sparseResidencyImage2D = true;
			else if (info->samples == VK_SAMPLE_COUNT_2_BIT) core10.sparseResidency2Samples = true;
			else if (info->samples == VK_SAMPLE_COUNT_4_BIT) core10.sparseResidency4Samples = true;
			else if (info->samples == VK_SAMPLE_COUNT_8_BIT) core10.sparseResidency8Samples = true;
			else if (info->samples == VK_SAMPLE_COUNT_16_BIT) core10.sparseResidency16Samples = true;
		}
		else if (info->imageType == VK_IMAGE_TYPE_3D && info->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) core10.sparseResidencyImage3D = true;

		if (info->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) core10.sparseBinding = true;
		if (info->flags & VK_IMAGE_CREATE_SPARSE_ALIASED_BIT) core10.sparseResidencyAliased = true;
		if ((info->usage & VK_IMAGE_USAGE_STORAGE_BIT) && info->samples != VK_SAMPLE_COUNT_1_BIT) core10.shaderStorageImageMultisample = true;
	}

	void check_VkBufferCreateInfo(const VkBufferCreateInfo* info)
	{
		if (info->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
		{
			core10.sparseBinding = true;
			core10.sparseResidencyBuffer = true;
		}
		if (info->flags & VK_BUFFER_CREATE_SPARSE_ALIASED_BIT) core10.sparseResidencyAliased = true;
	}

	void check_VkPipelineRasterizationStateCreateInfo(const VkPipelineRasterizationStateCreateInfo* info)
	{
		if (info->depthClampEnable == VK_TRUE) core10.depthClamp = true;
		if (info->polygonMode == VK_POLYGON_MODE_POINT || info->polygonMode == VK_POLYGON_MODE_LINE) core10.fillModeNonSolid = true;
	}

	void check_VkPipelineDepthStencilStateCreateInfo(const VkPipelineDepthStencilStateCreateInfo* info)
	{
		if (info->depthBoundsTestEnable == VK_TRUE) core10.depthBounds = true;
	}

	// --- Checking functions. Call these for all these Vulkan commands after they are successfully called, before returning. ---

	void check_vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
	{
		if (drawCount > 1) core10.multiDrawIndirect = true;
	}

	void check_vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
	{
		if (drawCount > 1) core10.multiDrawIndirect = true;
	}

	void check_vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags)
	{
		if (flags & VK_QUERY_CONTROL_PRECISE_BIT) core10.occlusionQueryPrecise = true;
	}

	void check_vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
	{
		core12.drawIndirectCount = true;
	}

	void check_vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
	{
		core12.drawIndirectCount = true;
	}

	void check_vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
	{
		core12.hostQueryReset = true;
	}

	void check_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
	{
		core13.dynamicRendering = true;
	}

	// --- Remove unused feature bits from these structures ---

	void adjust_VkPhysicalDeviceFeatures(VkPhysicalDeviceFeatures& incore10)
	{
		// Only turn off the features we have checking code for
		#define CHECK_FEATURE10(_x) if (!core10._x) incore10._x = false;
		CHECK_FEATURE10(dualSrcBlend);
		CHECK_FEATURE10(geometryShader);
		CHECK_FEATURE10(tessellationShader);
		CHECK_FEATURE10(sampleRateShading);
		CHECK_FEATURE10(depthClamp);
		CHECK_FEATURE10(samplerAnisotropy);
		CHECK_FEATURE10(fillModeNonSolid);
		CHECK_FEATURE10(depthBounds);
		CHECK_FEATURE10(pipelineStatisticsQuery);
		CHECK_FEATURE10(shaderStorageImageMultisample);
		CHECK_FEATURE10(logicOp);
		CHECK_FEATURE10(alphaToOne);
		CHECK_FEATURE10(sparseBinding);
		CHECK_FEATURE10(sparseResidencyBuffer);
		CHECK_FEATURE10(sparseResidencyImage2D);
		CHECK_FEATURE10(sparseResidencyImage3D);
		CHECK_FEATURE10(sparseResidency2Samples);
		CHECK_FEATURE10(sparseResidency4Samples);
		CHECK_FEATURE10(sparseResidency8Samples);
		CHECK_FEATURE10(sparseResidency16Samples);
		CHECK_FEATURE10(sparseResidencyAliased);
		#undef CHECK_FEATURE10
	}

	void adjust_VkPhysicalDeviceVulkan11Features(VkPhysicalDeviceVulkan11Features& incore11)
	{
	}

	void adjust_VkPhysicalDeviceVulkan12Features(VkPhysicalDeviceVulkan12Features& incore12)
	{
		// Only turn off the features we have checking code for
		#define CHECK_FEATURE12(_x) if (!core12._x) incore12._x = false;
		CHECK_FEATURE12(drawIndirectCount);
		CHECK_FEATURE12(hostQueryReset);
		CHECK_FEATURE12(samplerMirrorClampToEdge);
		#undef CHECK_FEATURE12
	}

	void adjust_VkPhysicalDeviceVulkan13Features(VkPhysicalDeviceVulkan13Features& incore13)
	{
		// Only turn off the features we have checking code for
		#define CHECK_FEATURE13(_x) if (!core13._x) incore13._x = false;
		CHECK_FEATURE13(dynamicRendering);
		#undef CHECK_FEATURE13
	}
};
