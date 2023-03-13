#include "feature_detect.h"
#include "util.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

int main()
{
	feature_detection detect;

	VkPhysicalDeviceFeatures feat10 = {};
	assert(feat10.logicOp == VK_FALSE); // not used
	VkPipelineColorBlendStateCreateInfo pipeinfo = {};
	detect.check_VkPipelineColorBlendStateCreateInfo(&pipeinfo);
	detect.adjust_VkPhysicalDeviceFeatures(feat10);
	assert(feat10.logicOp == VK_FALSE); // still not used

	feat10.logicOp = VK_TRUE; // set to used, but not actually used
	detect.adjust_VkPhysicalDeviceFeatures(feat10);
	assert(feat10.logicOp == VK_FALSE); // corretly corrected to not used

	feat10.logicOp = VK_TRUE; // set to used
	pipeinfo.logicOpEnable = VK_TRUE; // used here
	detect.check_VkPipelineColorBlendStateCreateInfo(&pipeinfo);
	detect.adjust_VkPhysicalDeviceFeatures(feat10);
	assert(feat10.logicOp == VK_TRUE); // not changed, still used

	VkPhysicalDeviceVulkan12Features feat12 = {};
	detect.adjust_VkPhysicalDeviceVulkan12Features(feat12);
	assert(feat12.drawIndirectCount == VK_FALSE);
	assert(feat12.hostQueryReset == VK_FALSE);
	feat12.drawIndirectCount = VK_TRUE;
	detect.adjust_VkPhysicalDeviceVulkan12Features(feat12);
	assert(feat12.drawIndirectCount == VK_FALSE); // was adjusted
	feat12.drawIndirectCount = VK_TRUE;
	detect.check_vkCmdDrawIndirectCount(0, 0, 0, 0, 0, 0, 0); // actually use feature
	detect.adjust_VkPhysicalDeviceVulkan12Features(feat12);
	assert(feat12.drawIndirectCount == VK_TRUE); // now unchanged
	assert(feat12.hostQueryReset == VK_FALSE); // also unchanged

	VkBaseOutStructure second = { (VkStructureType)2, nullptr };
	VkBaseOutStructure first = { (VkStructureType)1, &second };
	VkBaseOutStructure root = { (VkStructureType)0, &first };
	assert(find_extension_parent(&root, (VkStructureType)0) == nullptr);
	assert(find_extension_parent(&root, (VkStructureType)1) == &root);
	assert(find_extension_parent(&root, (VkStructureType)2) == &first);
	assert(find_extension(&root, (VkStructureType)0) == &root);
	assert(find_extension(&root, (VkStructureType)1) == &first);
	assert(find_extension(&root, (VkStructureType)2) == &second);

	return 0;
}
