#!/usr/bin/python2

import spec
import util

# Do not generate read/write functions for these
skiplist = [ 'VkXlibSurfaceCreateInfoKHR', 'VkXcbSurfaceCreateInfoKHR', 'VkBaseOutStructure', 'VkBaseInStructure', 'VkAllocationCallbacks',
	'VkDeviceFaultInfoEXT', 'VkMicromapBuildInfoEXT', 'VkAccelerationStructureTrianglesOpacityMicromapEXT', 'VkVideoDecodeH264ProfileInfoKHR',
	'VkVideoDecodeH264CapabilitiesKHR', 'VkVideoDecodeH264SessionParametersAddInfoKHR', 'VkVideoDecodeH264ProfileInfoKHR', 'VkVideoDecodeH264SessionParametersCreateInfoKHR',
	'VkVideoDecodeH264PictureInfoKHR', 'VkVideoDecodeH264DpbSlotInfoKHR', 'VkVideoDecodeH265ProfileInfoKHR', 'VkVideoDecodeH265SessionParametersAddInfoKHR',
	'VkVideoDecodeH265CapabilitiesKHR', 'VkVideoDecodeH265SessionParametersCreateInfoKHR', 'VkVideoDecodeH265PictureInfoKHR', 'VkVideoDecodeH265DpbSlotInfoKHR',
	'VkOpaqueCaptureDescriptorDataCreateInfoEXT' ]

hardcoded_read = [ 'VkAccelerationStructureBuildGeometryInfoKHR' ]
hardcoded_write = []

z = util.getspool()
structlist = []
for v in spec.root.findall('types/type'):
	name = v.attrib.get('name')
	category = v.attrib.get('category')
	if category != 'struct':
		continue
	if name in skiplist:
		continue
	if spec.str_contains_vendor(name) or not name in spec.types:
		continue
	structlist.append(name)

def struct_header_read(r, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if not name in structlist or (selected and name != selected) or name in util.struct_noop:
			continue
		if name in spec.protected_types:
			print >> r, '#ifdef %s' % spec.protected_types[name]
		structlist.append(name)
		accessor = '%s* sptr' % name
		print >> r, 'static void read_%s(lava_file_reader& reader, %s);' % (name, accessor)
		if name in spec.protected_types:
			print >> r, '#endif // %s' % spec.protected_types[name]
	print >> r

def struct_header_write(w, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if not name in structlist or (selected and name != selected) or name in util.struct_noop:
			continue
		if name in spec.protected_types:
			print >> w, '#ifdef %s' % spec.protected_types[name]
		structlist.append(name)
		accessor = '%s* sptr' % name
		print >> w, 'static void write_%s(lava_file_writer& writer, const %s);' % (name, accessor)
		if name in spec.protected_types:
			print >> w, '#endif // %s' % spec.protected_types[name]
	print >> w

def struct_impl_read(r, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if not name in structlist or (selected and name != selected) or name in util.struct_noop or name in hardcoded_read:
			continue
		if name in spec.protected_types:
			print >> r, '#ifdef %s' % spec.protected_types[name]
		accessor = '%s* sptr' % name
		special = ''
		if name == 'VkDeviceCreateInfo': special = ', VkPhysicalDevice physicalDevice'
		elif 'VkBindBufferMemoryInfo' in name or 'VkBindImageMemoryInfo' in name: special = ', VkDevice device'
		print >> r, 'static void read_%s(lava_file_reader& reader, %s%s)' % (name, accessor, special)
		print >> r, '{'
		if v.attrib.get('alias'):
			if 'VkDeviceCreateInfo' in name: special = ', physicalDevice'
			elif 'VkBindBufferMemoryInfo' in name or 'VkBindImageMemoryInfo' in name: special = ', device'
			print >> r, '\tread_%s(reader, sptr%s);' % (v.attrib.get('alias'), special)
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
			z.struct_end()
			z.dump()
		print >> r, '}'
		if name in spec.protected_types:
			print >> r, '#endif // %s' % spec.protected_types[name]
		print >> r

def struct_impl_write(w, selected = None):
	for v in spec.root.findall('types/type'):
		name = v.attrib.get('name')
		if not name in structlist or (selected and name != selected) or name in util.struct_noop or name in hardcoded_write:
			continue
		if name in spec.protected_types:
			print >> w, '#ifdef %s' % spec.protected_types[name]
		accessor = '%s* sptr' % name
		# Write implementation
		special = ''
		if name == 'VkPipelineViewportStateCreateInfo': special = ', const VkPipelineDynamicStateCreateInfo* pDynamicState'
		elif name == 'VkCommandBufferBeginInfo': special = ', trackedcmdbuffer_trace* tcmd'
		elif name == 'VkWriteDescriptorSet': special = ', bool ignoreDstSet'
		print >> w, 'static void write_%s(lava_file_writer& writer, const %s%s)' % (name, accessor, special)
		print >> w, '{'
		if v.attrib.get('alias'):
			print >> w, '\twrite_%s(writer, sptr);' % v.attrib.get('alias')
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
			z.struct_end()
			z.dump()
		print >> w, '}'
		# Done
		if name in spec.protected_types:
			print >> w, '#endif // %s' % spec.protected_types[name]
		print >> w

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

