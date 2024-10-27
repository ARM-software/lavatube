#!/usr/bin/python3

import sys
sys.path.append('external/tracetooltests/scripts')

import spec
import util

# Do not generate read/write functions for these
skiplist = [ 'VkXlibSurfaceCreateInfoKHR', 'VkXcbSurfaceCreateInfoKHR', 'VkBaseOutStructure', 'VkBaseInStructure', 'VkAllocationCallbacks',
	'VkDeviceFaultInfoEXT', 'VkMicromapBuildInfoEXT', 'VkAccelerationStructureTrianglesOpacityMicromapEXT', 'VkVideoDecodeH264ProfileInfoKHR',
	'VkVideoDecodeH264CapabilitiesKHR', 'VkVideoDecodeH264SessionParametersAddInfoKHR', 'VkVideoDecodeH264ProfileInfoKHR', 'VkVideoDecodeH264SessionParametersCreateInfoKHR',
	'VkVideoDecodeH264PictureInfoKHR', 'VkVideoDecodeH264DpbSlotInfoKHR', 'VkVideoDecodeH265ProfileInfoKHR', 'VkVideoDecodeH265SessionParametersAddInfoKHR',
	'VkVideoDecodeH265CapabilitiesKHR', 'VkVideoDecodeH265SessionParametersCreateInfoKHR', 'VkVideoDecodeH265PictureInfoKHR', 'VkVideoDecodeH265DpbSlotInfoKHR',
	'VkOpaqueCaptureDescriptorDataCreateInfoEXT',
	'VkPushDescriptorSetWithTemplateInfoKHR',
	'VkMemoryMapPlacedInfoEXT',
	'VkBindMemoryStatusKHR',
	'VkRenderingInputAttachmentIndexInfoKHR',
]

hardcoded_read = [ 'VkAccelerationStructureBuildGeometryInfoKHR' ]
hardcoded_write = [ 'VkUpdateMemoryInfoTRACETOOLTEST', 'VkAddressRemapTRACETOOLTEST', 'VkPatchChunkTRACETOOLTEST' ]

z = util.getspool()

def skip(name, selected):
	if not name in spec.structures or (selected and name != selected) or name in util.struct_noop:
		return True
	if name in skiplist:
		return True
	return False

def struct_header_read(r, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if skip(name, selected): continue
		if name in spec.protected_types:
			print('#ifdef %s' % spec.protected_types[name], file=r)
		accessor = '%s* sptr' % name
		print('static void read_%s(lava_file_reader& reader, %s);' % (name, accessor), file=r)
		if name in spec.protected_types:
			print('#endif // %s' % spec.protected_types[name], file=r)
	print(file=r)

def struct_header_write(w, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if skip(name, selected): continue
		if name in spec.protected_types:
			print('#ifdef %s' % spec.protected_types[name], file=w)
		accessor = '%s* sptr' % name
		modifier = 'const ' if not name in util.deconst_struct else ''
		print('static void write_%s(lava_file_writer& writer, %s%s);' % (name, modifier, accessor), file=w)
		if name in spec.protected_types:
			print('#endif // %s' % spec.protected_types[name], file=w)
	print(file=w)

def struct_add_tracking_read(name):
	if name in ['VkImageMemoryBarrier2', 'VkImageMemoryBarrier']:
		z.do('trackedimage& image_data = VkImage_index.at(image_index);')
		# TBD Fix this tracking code and reenable the below assert
		#z.do('assert(image_data.currentLayout == sptr->oldLayout);')
		z.do('image_data.currentLayout = sptr->newLayout;')

def struct_add_tracking_write(name):
	if name in ['VkImageMemoryBarrier2', 'VkImageMemoryBarrier']:
		# TBD Fix this tracking code and reenable the below assert
		#z.do('assert(image_data->currentLayout == sptr->oldLayout);')
		z.do('image_data->currentLayout = sptr->newLayout;')

def struct_impl_read(r, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if skip(name, selected) or name in hardcoded_read: continue
		if name in spec.protected_types:
			print('#ifdef %s' % spec.protected_types[name], file=r)
		accessor = '%s* sptr' % name
		special = ''
		if name == 'VkDeviceCreateInfo': special = ', VkPhysicalDevice physicalDevice'
		elif 'VkBindBufferMemoryInfo' in name or 'VkBindImageMemoryInfo' in name: special = ', VkDevice device'
		print('static void read_%s(lava_file_reader& reader, %s%s)' % (name, accessor, special), file=r)
		print('{', file=r)
		if v.attrib.get('alias'):
			if 'VkDeviceCreateInfo' in name: special = ', physicalDevice'
			elif 'VkBindBufferMemoryInfo' in name or 'VkBindImageMemoryInfo' in name: special = ', device'
			print('\tread_%s(reader, sptr%s);' % (v.attrib.get('alias'), special), file=r)
		else:
			z.target(r)
			z.read = True
			params = []
			z.struct_begin(name)
			for p in v.findall('member'):
				api = p.attrib.get('api')
				if api and api == 'vulkansc': continue
				param = util.parameter(p, read=True, funcname=name)
				param.print_load(param.name, 'sptr->')
			struct_add_tracking_read(name)
			z.struct_end()
			z.dump()
		print('}', file=r)
		if name in spec.protected_types:
			print('#endif // %s' % spec.protected_types[name], file=r)
		print(file=r)

def struct_impl_write(w, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if skip(name, selected) or name in hardcoded_write: continue
		if name in spec.protected_types:
			print('#ifdef %s' % spec.protected_types[name], file=w)
		accessor = '%s* sptr' % name
		# Write implementation
		special = ''
		if name == 'VkPipelineViewportStateCreateInfo': special = ', const VkPipelineDynamicStateCreateInfo* pDynamicState'
		elif name == 'VkCommandBufferBeginInfo': special = ', trackedcmdbuffer_trace* tcmd'
		elif name == 'VkWriteDescriptorSet': special = ', bool ignoreDstSet'
		modifier = 'const ' if not name in util.deconst_struct else ''
		print('static void write_%s(lava_file_writer& writer, %s%s%s)' % (name, modifier, accessor, special), file=w)
		print('{', file=w)
		if v.attrib.get('alias'):
			print('\twrite_%s(writer, sptr);' % v.attrib.get('alias'), file=w)
		else:
			z.target(w)
			z.read = False
			params = []
			z.struct_begin(name)
			if name == 'VkGraphicsPipelineCreateInfo':
				z.do('uint32_t stageFlags = 0;')
				z.do('for (uint32_t sf = 0; sf < sptr->stageCount; sf++) stageFlags |= (uint32_t)sptr->pStages[sf].stage;')
			elif name == 'VkPipelineViewportStateCreateInfo':
				z.do('bool isDynamicViewports = false;')
				z.do('bool isDynamicScissors = false;')
				z.do('for (uint32_t df = 0; pDynamicState && df < pDynamicState->dynamicStateCount; df++)')
				z.loop_begin()
				z.do('if (pDynamicState->pDynamicStates[df] == VK_DYNAMIC_STATE_VIEWPORT /*|| pDynamicState->pDynamicStates[df] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT*/) isDynamicViewports = true;')
				z.do('if (pDynamicState->pDynamicStates[df] == VK_DYNAMIC_STATE_SCISSOR /*|| pDynamicState->pDynamicStates[df] == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT*/) isDynamicScissors = true;')
				z.loop_end()
			for p in v.findall('member'):
				api = p.attrib.get('api')
				if api and api == 'vulkansc': continue
				param = util.parameter(p, read=False, funcname=name, transitiveConst=True)
				param.print_save(param.name, 'sptr->')
			if name in util.feature_detection_structs:
				z.do('writer.parent->usage_detection.check_%s(sptr);' % name)
			util.save_add_tracking(name)
			struct_add_tracking_write(name)
			z.struct_end()
			z.dump()
		print('}', file=w)
		# Done
		if name in spec.protected_types:
			print('#endif // %s' % spec.protected_types[name], file=w)
		print(file=w)

if __name__ == '__main__':
	r = open('generated/struct_read_auto.h', 'w')
	w = open('generated/struct_write_auto.h', 'w')
	struct_header_read(r)
	struct_header_write(w)
	r.close()
	w.close()
	r = open('generated/struct_read_auto.cpp', 'w')
	w = open('generated/struct_write_auto.cpp', 'w')
	struct_impl_read(r)
	struct_impl_write(w)
	r.close()
	w.close()
