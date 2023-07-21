#!/usr/bin/python2

import sys
sys.path.append('external/tracetooltests/scripts')

import xml.etree.ElementTree as ET
import re
import sys
import collections
import spec

spec.init()

feature_detection_structs = []
feature_detection_funcs = []
detect_words = []
with open('include/feature_detect.h', 'r') as f:
	for line in f:
		m = re.search('check_(\w+)', line)
		if m:
			detect_words.append(m.group(1))
for name in spec.structures:
	if name in detect_words:
		feature_detection_structs.append(name)

# Set this to zero to enable injecting sentinel values between each real value.
debugcount = -1

# Extra 'optional' variables to work around silly Vulkan API decisions
extra_optionals = {
	'VkWriteDescriptorSet': {
		'pImageInfo': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'pBufferInfo': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)',
		'pTexelBufferView': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)',
		'sampler': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)',
		'imageView': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'imageLayout': '(sptr->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || sptr->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || sptr->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)',
		'dstSet' : '!ignoreDstSet', # the actual condition is that the calling function is not vkCmdPushDescriptorSetKHR, which is specification insanity but there we go...
	},
	# we need to introduce a virtual parameter here that combine information here to express these dependencies, which is horrible but unavoidable
	# stageFlags, isDynamicViewports and isDynamicScissors are all our inventions
	'VkGraphicsPipelineCreateInfo': {
		'pTessellationState': '((stageFlags & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) && (stageFlags & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))',
		'pViewportState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)',
		'pMultisampleState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)',
		'pDepthStencilState' : '(!sptr->pRasterizationState->rasterizerDiscardEnable)', # "or if the subpass of the render pass the pipeline is created against does not use a depth/stencil attachment"
		'pColorBlendState': '(!sptr->pRasterizationState->rasterizerDiscardEnable)', # "or if the subpass of the render pass the pipeline is created against does not use any color attachments"
	},
	'VkPipelineViewportStateCreateInfo': {
		'pViewports': '(!isDynamicViewports)',
		'pScissors': '(!isDynamicScissors)',
	},
	'VkBufferCreateInfo': {
		'pQueueFamilyIndices': '(sptr->sharingMode == VK_SHARING_MODE_CONCURRENT)',
	},
	# this depends on state outside of the function parameters...
	'VkCommandBufferBeginInfo': {
		'pInheritanceInfo': '(tcmd->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)', # "If this is a primary command buffer, then this value is ignored."
	},
}

skip_opt_check = ['pAllocator', 'pUserData', 'pfnCallback', 'pfnUserCallback', 'pNext' ]
# for these, thread barrier goes before the function call to sync us up to other threads:
thread_barrier_funcs = [ 'vkQueueSubmit', 'vkResetDescriptorPool', 'vkResetCommandPool', 'vkUnmapMemory', 'vkFlushMappedMemoryRanges', 'vkResetQueryPool',
	'vkResetQueryPoolEXT', 'vkQueueSubmit2', 'vkQueueSubmit2EXT', 'vkQueuePresentKHR', 'vkFrameEndTRACETOOLTEST', 'vkUnmapMemory2KHR' ]
# for these, thread barrier goes after the function call to sync other threads up to us:
push_thread_barrier_funcs = [ 'vkQueueWaitIdle', 'vkDeviceWaitIdle', 'vkResetDescriptorPool', 'vkResetQueryPool', 'vkResetQueryPoolEXT', 'vkResetCommandPool',
	'vkQueuePresentKHR', 'vkWaitForFences', 'vkGetFenceStatus', 'vkFrameEndTRACETOOLTEST' ]

# TODO : Add support for these functions and structures
functions_noop = [
	"vkUpdateDescriptorSetWithTemplateKHR", "vkUpdateDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplateKHR", 'vkGetImageViewOpaqueCaptureDescriptorDataEXT',
	'vkGetPipelinePropertiesEXT', 'vkUpdateDescriptorSetWithTemplate', 'vkCmdUpdateBuffer', 'vkGetBufferOpaqueCaptureDescriptorDataEXT',
	'vkCmdBuildMicromapsEXT', 'vkBuildMicromapsEXT', 'vkGetMicromapBuildSizesEXT', 'vkGetImageOpaqueCaptureDescriptorDataEXT', 'vkGetSamplerOpaqueCaptureDescriptorDataEXT',
	'vkGetDeviceFaultInfoEXT', # we never want to trace this, but rather inject it during tracing if device loss happens, print the info, then abort
	'vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT'
]
struct_noop = []

# these should skip their native/upstream call and leave everything up to the post-execute callback
noscreen_calls = [ 'vkDestroySurfaceKHR', 'vkAcquireNextImageKHR', 'vkCreateSwapchainKHR', 'vkGetPhysicalDeviceSurfaceCapabilitiesKHR', 'vkAcquireNextImage2KHR',
	'vkGetPhysicalDeviceSurfacePresentModesKHR', 'vkGetPhysicalDeviceXlibPresentationSupportKHR', 'vkQueuePresentKHR', 'vkGetDeviceGroupPresentCapabilitiesKHR',
	'vkGetPhysicalDevicePresentRectanglesKHR', 'vkDestroySwapchainKHR', 'vkGetDeviceGroupSurfacePresentModesKHR', 'vkCreateSharedSwapchainsKHR',
	'vkGetSwapchainStatusKHR', 'vkGetSwapchainCounterEXT', 'vkGetRefreshCycleDurationGOOGLE', 'vkGetPastPresentationTimingGOOGLE', 'vkSetHdrMetadataEXT',
	'vkSetLocalDimmingAMD', 'vkGetPhysicalDeviceSurfaceCapabilities2EXT', 'vkGetPhysicalDeviceSurfaceFormats2KHR',
	'vkGetPhysicalDeviceSurfaceCapabilities2KHR', 'vkCreateDisplayPlaneSurfaceKHR', 'vkGetPhysicalDeviceSurfaceSupportKHR', 'vkGetPhysicalDeviceSurfaceFormatsKHR',
	'VkSwapchainCreateInfoKHR', 'vkAcquireFullScreenExclusiveModeEXT', 'vkReleaseFullScreenExclusiveModeEXT', 'vkWaitForPresentKHR' ]

# make sure we've thought about all the ways virtual swapchains interact with everything else
virtualswap_calls = [ 'vkCreateSwapchainKHR', 'vkDestroySwapchainKHR', 'vkCreateSharedSwapchainsKHR', 'vkGetSwapchainStatusKHR', 'vkGetSwapchainCounterEXT' ]

# Set this to true to generate packet statistics. The generated code will be quite a lot more bloated.
debugstats = False

# These functions are hard-coded in hardcoded_{write|read}.cpp
hardcoded = [ 'vkGetSwapchainImagesKHR', 'vkCreateAndroidSurfaceKHR', 'vkGetDeviceProcAddr',
	'vkGetInstanceProcAddr', 'vkCreateWaylandSurfaceKHR', 'vkCreateHeadlessSurfaceEXT', 'vkCreateXcbSurfaceKHR', 'vkCreateXlibSurfaceKHR',
	'vkDestroySurfaceKHR', 'vkGetDeviceQueue', 'vkGetDeviceQueue2', "vkGetAndroidHardwareBufferPropertiesANDROID", "vkGetMemoryAndroidHardwareBufferANDROID",
	'vkEnumerateInstanceLayerProperties', 'vkEnumerateInstanceExtensionProperties', 'vkEnumerateDeviceLayerProperties', 'vkEnumerateDeviceExtensionProperties',
	'vkGetPhysicalDeviceXlibPresentationSupportKHR', 'vkCreateWin32SurfaceKHR', 'vkCreateDirectFBSurfaceEXT', 'vkCreateMetalSurfaceEXT' ]
hardcoded_write = [ 'vkGetPhysicalDeviceToolPropertiesEXT', 'vkGetPhysicalDeviceToolProperties' ]
hardcoded_read = [ 'vkCmdBuildAccelerationStructuresIndirectKHR' ]
# For these functions it is ok if the function pointer is missing, since we implement them ourselves
layer_implemented = [ 'vkCreateDebugReportCallbackEXT', 'vkDestroyDebugReportCallbackEXT', 'vkDebugReportMessageEXT', 'vkDebugMarkerSetObjectTagEXT',
	'vkDebugMarkerSetObjectNameEXT', 'vkCmdDebugMarkerBeginEXT', 'vkCmdDebugMarkerEndEXT', 'vkCmdDebugMarkerInsertEXT', 'vkSetDebugUtilsObjectNameEXT',
	'vkSetDebugUtilsObjectTagEXT',	'vkQueueBeginDebugUtilsLabelEXT', 'vkQueueEndDebugUtilsLabelEXT', 'vkQueueInsertDebugUtilsLabelEXT',
	'vkCmdBeginDebugUtilsLabelEXT', 'vkCmdEndDebugUtilsLabelEXT', 'vkCmdInsertDebugUtilsLabelEXT', 'vkCreateDebugUtilsMessengerEXT',
	'vkDestroyDebugUtilsMessengerEXT', 'vkSubmitDebugUtilsMessageEXT', 'vkGetPhysicalDeviceToolPropertiesEXT', 'vkGetPhysicalDeviceToolProperties' ]
# functions we should ignore on replay
ignore_on_read = [ 'vkGetMemoryHostPointerPropertiesEXT', 'vkCreateDebugUtilsMessengerEXT', 'vkDestroyDebugUtilsMessengerEXT', 'vkAllocateMemory',
	'vkMapMemory', 'vkUnmapMemory', 'vkCreateDebugReportCallbackEXT', 'vkDestroyDebugReportCallbackEXT', 'vkFlushMappedMemoryRanges',
	'vkInvalidateMappedMemoryRanges', 'vkFreeMemory', 'vkGetPhysicalDeviceXcbPresentationSupportKHR', 'vkMapMemory2KHR', 'vkUnmapMemory2KHR',
	'vkGetImageMemoryRequirements2KHR', 'vkGetBufferMemoryRequirements2KHR', 'vkGetImageSparseMemoryRequirements2KHR', 'vkGetImageMemoryRequirements',
	'vkGetBufferMemoryRequirements', 'vkGetImageSparseMemoryRequirements', 'vkGetImageMemoryRequirements2', 'vkGetBufferMemoryRequirements2',
	'vkGetImageSparseMemoryRequirements2' ]
# functions we should not call natively when tracing - let pre or post calls handle it
ignore_on_trace = []
# these functions have hard-coded post-execute callbacks
replay_pre_calls = [ 'vkDestroyInstance', 'vkDestroyDevice', 'vkCreateDevice', 'vkCreateSampler', 'vkQueuePresentKHR', 'vkCreateSwapchainKHR',
	'vkCreateSharedSwapchainsKHR' ]
replay_post_calls = [ 'vkCreateInstance', 'vkCreateDevice', 'vkDestroyInstance', 'vkQueuePresentKHR', 'vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR' ]
trace_pre_calls = [ 'vkQueueSubmit', 'vkCreateInstance', 'vkCreateDevice', 'vkFreeMemory', 'vkQueueSubmit2', 'vkQueueSubmit2KHR' ]
trace_post_calls = [ 'vkCreateInstance', 'vkCreateDevice', 'vkDestroyInstance', 'vkGetPhysicalDeviceFeatures', 'vkGetPhysicalDeviceProperties',
		'vkGetPhysicalDeviceSurfaceCapabilitiesKHR', 'vkBindImageMemory', 'vkBindBufferMemory', 'vkBindImageMemory2', 'vkBindImageMemory2KHR',
		'vkBindBufferMemory2', 'vkUpdateDescriptorSets', 'vkFlushMappedMemoryRanges', 'vkQueuePresentKHR', 'vkMapMemory2KHR',
		'vkMapMemory', 'vkCmdBindDescriptorSets', 'vkBindBufferMemory2KHR', 'vkCmdPushDescriptorSet',
		'vkGetImageMemoryRequirements', 'vkGetPipelineCacheData', 'vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR',
		'vkGetBufferMemoryRequirements', 'vkGetBufferMemoryRequirements2', 'vkGetImageMemoryRequirements2', 'vkGetPhysicalDeviceMemoryProperties',
		'vkGetPhysicalDeviceFormatProperties', 'vkGetPhysicalDeviceFormatProperties2', 'vkCmdPushDescriptorSetKHR', 'vkCreateSwapchainKHR',
		'vkGetBufferMemoryRequirements2KHR', 'vkGetDeviceBufferMemoryRequirements', 'vkGetDeviceBufferMemoryRequirementsKHR',
		'vkGetDeviceImageMemoryRequirements', 'vkGetDeviceImageMemoryRequirementsKHR', 'vkGetPhysicalDeviceFeatures2', 'vkGetPhysicalDeviceFeatures2KHR',
		'vkGetPhysicalDeviceMemoryProperties2' ]
skip_post_calls = [ 'vkGetQueryPoolResults', 'vkGetPhysicalDeviceXcbPresentationSupportKHR' ]
# Awful workaround to be able to rewrite inputs while tracing: These input variables are copied and replaced to not be const anymore.
deconstify = {
	'vkAllocateMemory' : 'pAllocateInfo',
	'vkCreateInstance' : 'pCreateInfo',
	'vkCreateDevice' : 'pCreateInfo',
	'vkCreateSampler' : 'pCreateInfo',
	'vkCreateSwapchainKHR' : 'pCreateInfo',
}
# Subclassing of trackable
trackable_type_map_general = { 'VkBuffer': 'trackedbuffer', 'VkImage': 'trackedimage', 'VkCommandBuffer': 'trackedcmdbuffer', 'VkDescriptorSet': 'trackeddescriptorset',
	'VkDeviceMemory': 'trackedmemory', 'VkFence': 'trackedfence', 'VkPipeline': 'trackedpipeline', 'VkImageView': 'trackedimageview', 'VkBufferView': 'trackedbufferview',
	'VkDevice': 'trackeddevice', 'VkFramebuffer': 'trackedframebuffer', 'VkRenderPass': 'trackedrenderpass' }
trackable_type_map_trace = trackable_type_map_general.copy()
trackable_type_map_trace.update({ 'VkCommandBuffer': 'trackedcmdbuffer_trace', 'VkSwapchainKHR': 'trackedswapchain_trace', 'VkDescriptorSet': 'trackeddescriptorset_trace',
	'VkQueue': 'trackedqueue_trace', 'VkEvent': 'trackedevent_trace', 'VkDescriptorPool': 'trackeddescriptorpool_trace', 'VkCommandPool': 'trackedcommandpool_trace' })
trackable_type_map_replay = trackable_type_map_general.copy()
trackable_type_map_replay.update({ 'VkCommandBuffer': 'trackedcmdbuffer_replay', 'VkDescriptorSet': 'trackeddescriptorset_replay', 'VkSwapchainKHR': 'trackedswapchain_replay',
	'VkQueue': 'trackedqueue_replay' })

# Parse element size, which can be weird
def getraw(val):
	raw = ''
	for s in val.iter():
		if s.tag == 'enum':
			raw += s.text.strip()
		if s.tail:
			raw += s.tail.strip()
	return raw

def getsize(raw):
	if '[' in raw and not type == 'char':
		return re.search('.*\[(.+)\]', raw).group(1)
	return None

def typetmpname(root):
	assert not '[' in root, 'Bad name %s' % root
	return 'tmp_' + root[0] + root.translate(None, '_*-.:<> ')

# Used to split output into declarations and instructions. This is needed because
# we need things declared inside narrower scopes to outlive those scopes, and it
# is nice for reusing temporaries (some functions would use a lot of them).
class spool(object):
	def __init__(self):
		self.loops = 0
		self.declarations = []
		self.instructions = []
		self.before_instr = []
		self.indents = 1
		self.pmap = {}
		self.chain = [] # chain structures
		self.out = sys.stdout
		self.first_lines = [] # redefinitions of input parameters must be first of all

	def init(self, str):
		self.before_instr.append(str)

	def struct_begin(self, type):
		self.chain.append(type)

	def struct_end(self):
		self.chain.pop()

	def struct_last(self):
		return self.chain[-1]

	def decl(self, type, name, struct=False, custom=None):
		if type in ['uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'int']:
			assigned = '0'
		elif type in ['bool', 'VkBool32']:
			assigned = 'false'
		else:
			assigned = '(%s)0' % type

		if custom:
			assigned = custom
		elif 'void' in type or '*' in type:
			assigned = 'nullptr'
		elif struct:
			assigned = '{}'

		if 'std::vector' in type or 'std::string' in type or '[' in name or 'VkClearColorValue' in type:
			value = '%s %s;' % (type, name)
		else:
			value = '%s %s = %s;' % (type, name, assigned)

		for k in self.declarations: # deduplicate
			if value == k:
				return
		self.declarations.append(value)

	def do(self, str):
		if str[0] == '#': self.instructions.append(str) # no indentation for c macros
		else: self.instructions.append('\t'*self.indents + str)

	def brace_begin(self):
		self.do('{')
		self.indents += 1

	def brace_end(self):
		self.indents -= 1
		self.do('}')

	def first(self, value):
		self.first_lines.append(value)

	def dump(self):
		for v in self.first_lines:
			print >> self.out, '\t' + v
		if len(self.declarations) > 0:
			print >> self.out, '\t// -- Declarations --'
		for v in self.declarations:
			print >> self.out, '\t' + v
		if len(self.before_instr) > 0:
			print >> self.out, '\t// -- Initializations --'
		for v in self.before_instr:
			print >> self.out, '\t' + v
		if len(self.instructions) > 0 and (len(self.before_instr) > 0 or len(self.declarations) > 0):
			print >> self.out, '\t// -- Instructions --'
		for v in self.instructions:
			print >> self.out, v
		self.declarations = []
		self.instructions = []
		self.indents = 1
		self.pmap = {}
		self.before_instr = []
		self.first_lines = []

	# Change replay parameter signature. Only works if we call it
	# before we call param() when printing the execute call, obviously.
	def access(self, name, param):
		self.pmap[name] = param

	def param(self, name):
		if name in self.pmap: return self.pmap[name]
		else: return None

	# Set target to write to
	def target(self, t):
		self.out = t

	# For temporary arrays on the memory pool. Only usable in the replayer.
	def tmpmem(self, type, size):
		tmpname = typetmpname(type) + '_ptr'
		self.decl(type + '*', tmpname)
		self.do('%s = reader.pool.allocate<%s>(%s);' % (tmpname, type, size))
		return tmpname

	# For temporary variables with short lifetimes. Returns the name
	# of the temporary. Used to reuse variable declarations.
	def tmp(self, type, size=None):
		tmpname = typetmpname(type)
		self.decl(type, tmpname)
		return tmpname

	def loop_begin(self):
		self.brace_begin()
		self.loops += 1

	def loop_end(self):
		self.brace_end()
		self.loops -= 1

	# Generate enough variants of a backing store in case of loops or struct variants
	# Only usable in the replayer.
	def backing(self, type, varname, size='1', struct=False, clear=True):
		assert not '<' in varname
		assert not '[' in varname
		assert '[' not in type and '<' not in type
		name = varname + '_backing'
		if not size: size = '1'
		value = '%s* %s = nullptr;' % (type, name)
		for k in self.declarations: # deduplicate
			if value == k:
				break
		else:
			self.declarations.append(value)
		self.do('%s = reader.pool.allocate<%s>(%s);' % (name, type, size))
		if clear:
			self.do('memset(%s, 0, %s * sizeof(%s));' % (name, size, type))
		if clear and type in spec.type2sType:
			# this hack is needed in a very few cases, and is mostly useless as it is overwritten later anyways
			if size == '1':
				self.do('%s->sType = %s;' % (name, spec.type2sType[type]))
			else:
				self.do('for (unsigned sidx = 0; sidx < %s; sidx++) %s[sidx].sType = %s;' % (size, name, spec.type2sType[type]))
		return name

z = spool()

def getspool():
	return z

def isdebugcount():
	return debugcount

def toindex(type):
	return type.replace('Vk', '').lower() + '_index'

def totrackable(type):
	return type.replace('Vk', '').lower() + '_data'

# If this variable is a 'count' of another variable
def iscount(f):
	if f.type == 'VkSampleCountFlagBits' and f.name == 'rasterizationSamples':
		return True
	keys = [ 'dataSize', 'tagSize', 'initialDataSize', 'codeSize' ]
	return (f.type in ['uint32_t', 'size_t'] and ('Count' in f.name or f.name in keys) and not f.ptr and not f.length) or (f.funcname in spec.other_counts and f.name in spec.other_counts[f.funcname])

def isptr(n):
	return ('.' in n or '->' in n)

class parameter(object):
	def __init__(self, node, read, funcname, transitiveConst = False):
		raw = getraw(node)
		self.funcname = funcname
		self.name = node.find('name').text
		self.type = node.find('type').text
		self.mod = node.text
		if not self.mod:
			self.mod = ''
		self.const = ('const' in self.mod) or transitiveConst
		if self.const:
			self.mod = 'const '
		self.ptr = '*' in raw # is it a pointer?
		self.param_ptrstr = '* ' if self.ptr else ' '
		self.inline_ptrstr = '* ' if self.ptr else ''
		if '**' in raw:
			self.param_ptrstr = '** '
		if '* const*' in raw:
			self.param_ptrstr = '* const* '
		self.length = node.attrib.get('len') # eg 'null-terminated' or parameter name
		altlen = node.attrib.get('altlen') # alternative to latex stuff
		self.fixedsize = False
		if altlen:
			self.length = altlen
		if self.length:
			self.length = self.length.replace(',1', '')
		if self.length and '::' in self.length:
			self.length = self.length.replace('::','->')
		if not self.length and '[' in raw: # ie fixed length array
			length = node.find('enum')
			if length is not None:
				self.length = length.text
			else: # naked numeral
				self.length = getsize(raw)
		if self.length and (self.length.isdigit() or self.length.isupper()):
			self.fixedsize = True # fixed length array
		self.structure = self.type in spec.structures
		self.disphandle = self.type in spec.disp_handles
		self.nondisphandle = self.type in spec.nondisp_handles
		self.inparam = (not self.ptr or self.const) # else out parameter
		self.string_array = self.length and ',null-terminated' in self.length and self.type == 'char'
		self.string = self.length == 'null-terminated' and self.type == 'char'
		self.read = read

		# We need to be really defensive and treat all pointers as potentially optional. We cannot trust the optional XML
		# attribute since Khronos in their infinite wisdom have been turning formerly non-optional pointers into optional
		# pointers over time, which would break traces. Optional handles are handled separately (but are also all considered
		# potentially optional even though not set here).
		self.optional = self.ptr
		if self.ptr and (self.disphandle or self.nondisphandle) and not self.length: # pointers to single handles
			self.optional = False
		if funcname in extra_optionals and self.name in extra_optionals[funcname]:
			assert self.optional or self.disphandle or self.nondisphandle, '%s in %s is not marked as optional!' % (self.name, funcname)
		if self.string or self.string_array: # handle string optionalness specially
			self.optional = False

	def parameter(self):
		if self.length and not self.ptr:
			return ('%s%s%s%s[%s]' % (self.mod, self.type, self.param_ptrstr, self.name, self.length)).strip()
		return ('%s%s%s%s' % (self.mod, self.type, self.param_ptrstr, self.name)).strip()

	# Generate trace execution call parameters
	def trace_exec_param(self, funcname):
		global z

		if z.param(self.name):
			return z.param(self.name)
		else:
			return self.name

	# Generate retrace execution call parameters
	def retrace_exec_param(self, funcname):
		global z

		if z.param(self.name):
			return z.param(self.name)
		elif funcname in spec.functions_create and self.name == spec.functions_create[funcname][0] and spec.functions_create[funcname][1] == '1':
			assert self.name[0] != 'p' or self.name[1] != 'p', 'tried to put a & on %s - a %s!' % (self.name, self.type)
			return '&%s' % self.name
		elif funcname in spec.functions_create and self.name == spec.functions_create[funcname][0]: # multiple
			return '%s' % self.name
		elif self.type in ['VkClearColorValue', 'VkClearValue', 'VkPipelineExecutableStatisticValueKHR']:
			return '&%s' % self.name
		elif self.ptr and not self.structure:
			return '%s' % self.name
		else:
			return '%s' % self.name

	def print_loop(self, owner, varname, size, tmpname):
		if len(owner) > 0 and not size.isupper() and not size.isdigit():
			z.do('for (unsigned %s = 0; %s < %s; %s++) // varname=%s' % (tmpname, tmpname, owner + size, tmpname, varname))
		elif not self.read and size[0] == 'p' and size[1].isupper(): # eg pCreateInfoCount
			z.do('for (unsigned %s = 0; %s < *%s; %s++) // varname=%s' % (tmpname, tmpname, size, tmpname, varname))
		else:
			z.do('for (unsigned %s = 0; %s < %s; %s++) // varname=%s' % (tmpname, tmpname, size, tmpname, varname))
		if self.name[0] == 'p' and self.name[1] == 'p': return '%s[%s]' % (varname, tmpname)
		return '&%s[%s]' % (varname, tmpname)

	def print_struct(self, mytype, varname, owner, size = None):
		global z

		accessor = varname
		if size:
			accessor = self.print_loop(owner, varname, size, 'sidx')
			z.loop_begin()
		elif not self.ptr:
			assert self.name[0] != 'p' or self.name[1] != 'p', 'tried to put a & on %s - a %s!' % (self.name, self.type)
			accessor = '&' + varname

		# Stupid exceptions where Vulkan depends on out-of-struct state for allowing garbage values in struct member pointers
		if mytype == 'VkPipelineViewportStateCreateInfo' and not self.read: accessor += ', sptr->pDynamicState'
		elif mytype == 'VkCommandBufferBeginInfo' and not self.read: accessor += ', commandbuffer_data'
		elif 'vkCmdPushDescriptorSet' in self.funcname and mytype == 'VkWriteDescriptorSet' and not self.read: accessor += ', true'
		elif mytype == 'VkWriteDescriptorSet' and not self.read: accessor += ', false'
		elif mytype == 'VkDeviceCreateInfo' and self.read: accessor += ', physicalDevice'
		elif ('VkBindImageMemoryInfo' in mytype or 'VkBindBufferMemoryInfo' in mytype) and self.read: accessor += ', device'

		z.do('%s_%s(%s, %s);' % (('read' if self.read else 'write'), mytype, ('reader' if self.read else 'writer'), accessor))
		if size:
			z.loop_end()

	def print_load(self, name, owner): # called for each parameter
		global z
		global debugcount

		varname = owner + name
		is_root = not isptr(varname)

		if not self.funcname in noscreen_calls and not self.funcname in virtualswap_calls and self.funcname[0] == 'v':
			assert self.type != 'VkSwapchainKHR', '%s has VkSwapchainKHR in %s' % (self.funcname, self.name)
		if not self.funcname in noscreen_calls and self.funcname[0] == 'v':
			assert self.type != 'VkSurfaceKHR', '%s has VkSurfaceKHR in %s' % (self.funcname, self.name)

		if self.optional and not self.name in skip_opt_check:
			z.do('%s = reader.read_uint8_t(); // whether we should load %s' % (z.tmp('uint8_t'), self.name))
			z.do('if (%s)' % z.tmp('uint8_t'))
			z.brace_begin()

		if self.name == 'pAllocator':
			z.decl('VkAllocationCallbacks', 'allocator', struct=True)
			z.decl('VkAllocationCallbacks*', 'pAllocator', custom='&allocator')
			z.do('allocators_set(pAllocator);')
		elif self.name in ['pUserData']:
			pass
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT'] and self.name == 'objectHandle':
			z.do('%s = reader.read_handle();' % varname)
		elif self.type == 'VkAccelerationStructureBuildRangeInfoKHR':
			assert(self.funcname == 'vkBuildAccelerationStructuresKHR' or self.funcname == 'vkCmdBuildAccelerationStructuresKHR')
			z.decl(self.type + '**', self.name)
			z.do('%s = reader.pool.allocate<VkAccelerationStructureBuildRangeInfoKHR*>(infoCount);' % varname)
			z.do('for (unsigned i = 0; i < infoCount; i++) %s[i] = reader.pool.allocate<VkAccelerationStructureBuildRangeInfoKHR>(pInfos[i].geometryCount);' % varname)
			z.do('for (unsigned i = 0; i < infoCount; i++) for (unsigned j = 0; j < pInfos[i].geometryCount; j++) { auto* p = %s[i]; read_VkAccelerationStructureBuildRangeInfoKHR(reader, &p[j]); }' % varname)
		#elif self.name == 'queueFamilyIndex':
		#	z.decl('uint32_t', self.name)
		#	z.do('%s = selected_queue_family_index;' % self.name)
		#	z.do('(void)reader.read_uint32_t(); // ignore stored %s' % self.name)
		#	if not is_root:
		#		z.do('%s = %s;' % (varname, self.name))
		elif (self.name == 'ppData' and self.funcname in ['vkMapMemory', 'vkMapMemory2KHR']) or self.name == 'pHostPointer':
			z.decl('%s%s%s' % (self.mod, self.type, self.param_ptrstr), self.name)
		elif self.name == 'pfnUserCallback' and self.funcname == 'VkDebugUtilsMessengerCreateInfoEXT':
			z.do('%s = messenger_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pfnUserCallback' and self.funcname == 'VkDeviceDeviceMemoryReportCreateInfoEXT':
			z.do('%s = memory_report_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pfnCallback':
			z.do('%s = debug_report_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pNext':
			z.do('read_extension(reader, (VkBaseOutStructure**)&%s);' % varname)
		elif self.string_array:
			len = self.length.split(',')[0]
			if self.funcname == 'VkInstanceCreateInfo' and self.name == 'ppEnabledExtensionNames':
				z.do('%s = instance_extensions(reader, sptr->%s);' % (varname, len))
			elif self.funcname == 'VkInstanceCreateInfo' and self.name == 'ppEnabledLayerNames':
				z.do('%s = instance_layers(reader, sptr->%s);' % (varname, len))
			elif self.funcname == 'VkDeviceCreateInfo' and self.name == 'ppEnabledExtensionNames':
				z.do('%s = device_extensions(reader, physicalDevice, sptr->%s);' % (varname, len))
			elif self.funcname == 'VkDeviceCreateInfo' and self.name == 'ppEnabledLayerNames':
				z.do('%s = device_layers(reader, sptr->%s);' % (varname, len))
			else:
				z.do('%s = reader.read_string_array(%s);' % (varname, len))
		elif self.string:
			if not isptr(varname):
				z.decl('const char*', self.name)
				z.access(self.name, self.name)
			z.do('%s = reader.read_string();' % varname)
		elif self.structure:
			if is_root:
				z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
			if self.ptr:
				if not is_root and iscount(self):
					z.decl('%s%s*' % (self.mod, self.type), self.name)
					z.do('%s = %s;' % (self.name, varname))
				vname = z.backing(self.type, self.name, size=self.length, struct=True)
				z.do('%s = %s;' % (varname, vname))
				self.print_struct(self.type, vname, owner, size=self.length)
			elif not is_root:
				self.print_struct(self.type, varname, owner, size=self.length)
			else:
				self.print_struct(self.type, varname, owner, size=self.length)
		elif self.nondisphandle or self.disphandle:
			if self.funcname in spec.functions_create and self.name == spec.functions_create[self.funcname][0]:
				pass # handled elsewhere, because for out-params we need to process them before the execute is printed out
			elif self.length:
				z.do('if (%s > 0)' % self.length)
				z.brace_begin()
				storedname = z.tmpmem('uint32_t', self.length)
				z.do('reader.read_handle_array(%s, %s); // read unique indices to objects' % (storedname, self.length))
				usename = varname
				if not isptr(varname) or self.ptr:
					nativename = z.backing(self.type, self.name, size=self.length)
					z.do('%s = %s;' % (varname, nativename))
					if is_root:
						z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
						z.access(self.name, self.name) # to avoid &
					usename = nativename
				if self.funcname == 'VkSubmitInfo' and self.name == 'pCommandBuffers':
					z.do('if (is_blackhole_mode()) { %s = nullptr; sptr->commandBufferCount = 0; }' % varname)
				z.do('for (unsigned li2 = 0; li2 < %s; li2++) %s[li2] = index_to_%s.at(%s[li2]);' % (self.length, usename, self.type, storedname))
				if self.funcname in ['vkFreeDescriptorSets', 'vkFreeDescriptorSets', 'vkFreeCommandBuffers']:
					z.do('for (unsigned li2 = 0; li2 < %s; li2++) index_to_%s.unset(%s[li2]);' % (self.length, self.type, storedname))
				z.brace_end()
			elif self.ptr:
				tmpname = toindex(self.type)
				z.decl('uint32_t', tmpname)
				z.decl(self.type + '*', self.name)
				z.do('%s = reader.read_handle();' % tmpname)
				z.do('*%s = index_to_%s.at(%s);' % (varname, self.type, tmpname))
			else:
				if not isptr(varname):
					z.decl(self.type, self.name)
				tmpname = toindex(self.type)
				z.decl('uint32_t', tmpname)
				z.do('%s = reader.read_handle();' % tmpname)
				if self.type == 'VkPhysicalDevice':
					z.do('%s = selected_physical_device;' % varname)
				elif self.type != 'VkDeviceMemory' and not self.funcname in ignore_on_read:
					z.do('%s = index_to_%s.at(%s);' % (varname, self.type, tmpname))
		elif self.type == 'VkDeviceOrHostAddressKHR' or self.type == 'VkDeviceOrHostAddressConstKHR':
			z.do('%s.deviceAddress = reader.read_uint64_t();' % (varname))
		elif self.type == 'VkAccelerationStructureGeometryDataKHR': # union, requires special handling
			z.do('if (%sgeometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) read_VkAccelerationStructureGeometryTrianglesDataKHR(reader, &%s.triangles);' % (owner, varname))
			z.do('else if (%sgeometryType == VK_GEOMETRY_TYPE_AABBS_KHR) read_VkAccelerationStructureGeometryAabbsDataKHR(reader, &%s.aabbs);' % (owner, varname))
			z.do('else if (%sgeometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) read_VkAccelerationStructureGeometryInstancesDataKHR(reader, &%s.instances);' % (owner, varname))
			z.do('else assert(false);') # if geometryType is defined after VkAccelerationStructureGeometryDataKHR we have a problem
		elif self.type == 'VkPipelineExecutableStatisticValueKHR': # union, requires special handling
			z.do('if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR) %s.b32 = reader.read_uint32_t();' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) %s.i64 = reader.read_int64_t();' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) %s.u64 = reader.read_uint64_t();' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) %s.f64 = reader.read_double();' % (owner, varname))
			z.do('else assert(false);')
		elif self.type == 'VkClearColorValue': # union, requires special handling
			if not isptr(varname):
				z.decl(self.type, self.name)
			z.do('reader.read_array(%s.float32, 4); // read VkClearColorValue' % varname)
		elif self.type == 'VkClearValue': # union, requires special handling - _one_ case with a pointer array
			if not self.length:
				self.length = '1'
			else:
				self.length = owner + self.length
			if not self.ptr:
				z.do('%s %s;' % (self.type, self.name))
				z.do('reader.read_array((uint32_t*)&%s, 4 * %s); // read VkClearValue' % (varname, self.length))
			else:
				storedname = z.tmpmem('float', '%s * 4' % self.length)
				z.do('reader.read_array(%s, %s * 4); // read VkClearValue into temporary' % (storedname, self.length))
				z.do('%s = reinterpret_cast<VkClearValue*>(%s);' % (varname, storedname))
		elif self.type in spec.type_mappings:
			storedtype = spec.type_mappings[self.type]
			nativetype = self.type if self.type != 'void' else 'char'
			if self.length and self.ptr:
				len = z.tmp('uint32_t')
				z.do('%s = %s;' % (len, self.length))
				z.do('if (%s > 0)' % len)
				z.brace_begin()
				if not isptr(varname):
					z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
					z.access(self.name, self.name)
				storedname = z.tmpmem(storedtype, len)
				nativename = z.backing(nativetype, self.name, size=len)
				z.do('reader.read_array(%s, %s);' % (storedname, len))
				z.do('for (size_t k1 = 0; k1 < %s; k1++) %s[k1] = static_cast<%s>(%s[k1]);' % (len, nativename, nativetype, storedname))
				z.do('%s = %s;' % (varname, nativename))
				z.brace_end()
				z.do('else %s = nullptr;' % varname)
			elif self.length and not self.ptr:
				if not isptr(varname):
					z.decl(self.type, '%s[%s]' % (self.name, self.length))
				storedname = z.tmpmem(storedtype, self.length)
				z.do('reader.read_array(%s, %s);' % (storedname, self.length))
				z.do('for (size_t k2 = 0; k2 < %s; k2++) %s[k2] = static_cast<%s>(%s[k2]);' % (self.length, varname, self.type, storedname))
			elif not self.ptr and self.length:
				if not isptr(varname):
					z.decl(self.type, '%s[%d]' % (self.name, self.length))
				storedname = z.tmpmem(storedtype, str(self.length))
				z.do('reader.read_array(%s, %d);' % (storedname, self.length))
				z.do('for (size_t k3 = 0; k3 < %d; k3++) %s[k3] = static_cast<%s>(%s[k3]);' % (self.length, varname, self.type, storedname))
			else:
				if self.type == 'void':
					z.do('%s = reader.read_%s(); // for %s' % (z.tmp(storedtype), storedtype, varname))
					z.do('%s = nullptr;' % varname)
				elif is_root and self.ptr:
					z.decl(self.type + '*', self.name)
					z.do('*%s = static_cast<%s>(reader.read_%s());' % (self.name, self.type, storedtype))
					if isptr(varname):
						z.do('%s = *%s;' % (varname, self.name))
				elif iscount(self): # need to store in temporary in case used by other variables as size
					storedname = z.tmp(storedtype)
					z.do('%s = reader.read_%s(); // for %s' % (storedname, storedtype, varname))
					z.decl(self.type, self.name)
					z.do('%s = static_cast<%s>(%s); // tmp assign' % (self.name, self.type, storedname))
					if self.ptr:
						z.access(self.name, '&' + self.name)
					if isptr(varname):
						z.do('%s = %s;' % (varname, self.name))
				else: # no need, assign directly
					if is_root:
						z.decl('%s%s' % (self.type, self.inline_ptrstr), varname)
					z.do('%s = static_cast<%s>(reader.read_%s());' % (varname, self.type, storedtype))
		elif self.ptr and self.length: # pointer arrays
			z.do('if (%s > 0)' % self.length)
			z.brace_begin()
			vname = z.backing(self.type, self.name, self.length)
			z.do('reader.read_array(%s, %s); // array of dynamic length' % (vname, self.length))
			if is_root:
				z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
				z.access(self.name, self.name) # avoid a & being add
			z.do('%s = %s;' % (varname, vname))
			z.brace_end()
			if not is_root:
				z.do('else %s = nullptr;' % varname)
		elif not self.ptr and self.length: # specific size arrays
			if not isptr(varname):
				z.decl(self.type, '%s[%s]' % (self.name, self.length))
			z.do('reader.read_array(%s, %s); // array of specific size' % (varname, self.length))
		elif isptr(varname):
			# direct assignment to struct member for less clutter and stack memory consumption
			if iscount(self):
				z.decl(self.type, self.name)
				z.do('%s = reader.read_%s(); // indirect read because it is a count' % (self.name, self.type))
				z.do('%s = %s;' % (varname, self.name))
			else:
				z.do('%s = reader.read_%s();' % (varname, self.type))
		elif self.ptr:
			if is_root or iscount(self): # need to store temporary in case used by other variables as size
				z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
			z.do('*%s = reader.read_%s();' % (varname, self.type))
		else:
			if is_root or iscount(self): # need to store temporary in case used by other variables as size
				z.decl(self.type, self.name)
			z.do('%s = reader.read_%s();' % (varname, self.type))

		if self.funcname in ['vkBindImageMemory', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo'] and self.name == 'image':
			z.do('const VkMemoryPropertyFlags special_flags = static_cast<VkMemoryPropertyFlags>(reader.read_uint32_t()); // fetch memory flags especially added')
			z.do('const VkImageTiling tiling = static_cast<VkImageTiling>(reader.read_uint32_t()); // fetch tiling property especially added')
			z.do('const VkDeviceSize min_size = static_cast<VkDeviceSize>(reader.read_uint64_t()); // fetch padded memory size')
			z.do('suballoc_location loc = suballoc_add_image(reader.thread_index(), device, %s, image_index, special_flags, tiling, min_size);' % varname)
		elif self.funcname in ['vkBindBufferMemory', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR'] and self.name == 'buffer':
			z.do('const VkMemoryPropertyFlags special_flags = static_cast<VkMemoryPropertyFlags>(reader.read_uint32_t()); // fetch memory flags especially added')
			z.do('trackedbuffer& buffer_data = VkBuffer_index.at(buffer_index);')
			z.do('suballoc_location loc = suballoc_add_buffer(reader.thread_index(), device, %s, buffer_index, special_flags, buffer_data.usage);' % varname)

		if self.funcname in ['vkBindImageMemory', 'vkBindBufferMemory', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo']:
			if self.name == 'memory':
				z.do('assert(loc.memory != VK_NULL_HANDLE);')
				z.do('%s = loc.memory;' % varname) # relying on the order of arguments here; see case above
			elif self.name == 'memoryOffset':
				z.do('%s = loc.offset;' % varname) # relying on the order of arguments here; see case above

		if self.funcname == 'vkDestroyBuffer' and self.name == 'buffer':
			z.do('if (buffer_index != CONTAINER_INVALID_INDEX) suballoc_del_buffer(buffer_index);')
		elif self.funcname == 'vkDestroyImage' and self.name == 'image':
			z.do('suballoc_del_image(image_index);')
		elif self.name == 'sType':
			orig = z.struct_last()
			stype = spec.type2sType[orig]
			z.do('assert(%s == %s);' % (varname, stype))
		elif self.funcname == 'vkDestroySurface' and self.name == 'surface':
			z.do('window_destroy(instance, surfacekhr_index);')
		elif self.funcname in ['VkDebugMarkerObjectNameInfoEXT', 'VkDebugMarkerObjectTagInfoEXT', 'vkDebugReportMessageEXT'] and self.name == 'object':
			z.do('%s = debug_object_lookup(%sobjectType, %s);' % (varname, owner, varname))
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT'] and self.name == 'objectHandle':
			z.do('%s = object_lookup(%sobjectType, %s);' % (varname, owner, varname))

		if self.funcname in spec.functions_destroy and self.name == spec.functions_destroy[self.funcname][0] and spec.functions_destroy[self.funcname][1] == '1' and self.funcname not in ['vkFreeMemory']:
			param = spec.functions_destroy[self.funcname][0]
			type = spec.functions_destroy[self.funcname][2]
			if self.funcname not in ignore_on_read:
				z.do('if (%s != VK_NULL_HANDLE) index_to_%s.unset(%s);' % (varname, type, toindex(type)))

		if self.optional and not self.name in skip_opt_check:
			z.brace_end()

		if debugcount >= 0 and self.funcname != 'vkDestroyInstance':
			z.do('{ uint16_t _sentinel = reader.read_uint16_t(); assert(_sentinel == %d); } // sentinel for %s' % (debugcount, varname))
			debugcount += 1

	def print_save(self, name, owner): # called for each parameter
		global z
		global debugcount

		varname = owner + name
		is_root = not isptr(varname)

		if not is_root and iscount(self):
			z.decl('%s%s%s' % (self.mod if self.structure else '', self.type, self.param_ptrstr), self.name)
			z.do('%s = %s; // in case used elsewhere as a count' % (self.name, varname))

		if self.optional and not self.name in skip_opt_check:
			z.decl('uint8_t', '%s_opt' % self.name)
			if self.funcname in extra_optionals and self.name in extra_optionals[self.funcname]:
				z.do('%s_opt = %s && %s;' % (self.name, extra_optionals[self.funcname][self.name], varname))
			elif self.length:
				z.do('%s_opt = (%s != 0 && %s > 0); // whether we should save %s' % (self.name, varname, self.length, self.name))
			else:
				z.do('%s_opt = (%s != 0); // whether we should save this optional value' % (self.name, varname))
			z.do('writer.write_uint8_t(%s_opt);' % self.name)
			z.do('if (%s_opt)' % self.name)
			z.brace_begin()

		if self.name in ['pAllocator', 'pUserData', 'pfnCallback', 'pfnUserCallback']:
			pass
		elif (self.name == 'initialDataSize' and self.funcname == 'VkPipelineCacheCreateInfo'):
			z.do('writer.write_uint64_t(0); // initialDataSize : never write pipeline cache data into the trace itself')
			z.do('initialDataSize = 0;')
		elif (self.name == 'pInitialData' and self.funcname == 'VkPipelineCacheCreateInfo'):
			z.do('assert(false); // pInitialData')
		elif (self.name == 'ppData' and self.funcname in ['vkMapMemory', 'vkMapMemory2KHR']) or self.name == 'pHostPointer':
			pass
		elif self.structure:
			self.print_struct(self.type, varname, owner, size=self.length)
		elif self.funcname in ['VkDebugMarkerObjectNameInfoEXT', 'VkDebugMarkerObjectTagInfoEXT', 'vkDebugReportMessageEXT'] and self.name == 'object':
			z.do('auto* object_data = debug_object_trackable(writer.parent->records, %sobjectType, %s);' % (owner, varname))
			z.do('writer.write_handle(object_data);')
		elif 'CaptureReplayHandle' in self.name:
			z.do('writer.write_uint64_t((uint64_t)%s);' % varname)
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT'] and self.name == 'objectHandle':
			z.do('auto* object_data = object_trackable(writer.parent->records, %sobjectType, %s);' % (owner, varname))
			z.do('writer.write_handle(object_data);')
		elif self.name == 'pNext':
			z.do('write_extension(writer, (VkBaseOutStructure*)%s);' % varname)
		elif self.type == 'VkDeviceOrHostAddressKHR' or self.type == 'VkDeviceOrHostAddressConstKHR':
			z.do('writer.write_uint64_t(%s.deviceAddress);' % (varname))
		elif self.type == 'VkAccelerationStructureGeometryDataKHR': # union, requires special handling
			z.do('if (%sgeometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) write_VkAccelerationStructureGeometryTrianglesDataKHR(writer, &%s.triangles);' % (owner, varname))
			z.do('else if (%sgeometryType == VK_GEOMETRY_TYPE_AABBS_KHR) write_VkAccelerationStructureGeometryAabbsDataKHR(writer, &%s.aabbs);' % (owner, varname))
			z.do('else if (%sgeometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) write_VkAccelerationStructureGeometryInstancesDataKHR(writer, &%s.instances);' % (owner, varname))
			z.do('else assert(false);')
		elif self.type == 'VkPipelineExecutableStatisticValueKHR': # union, requires special handling
			z.do('if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR) writer.write_uint32_t(%s.b32);' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) writer.write_int64_t(%s.i64);' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) writer.write_uint64_t(%s.u64);' % (owner, varname))
			z.do('else if (%sformat == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) writer.write_double(%s.f64);' % (owner, varname))
			z.do('else assert(false);')
		elif self.type == 'VkClearColorValue': # union, requires special handling
			if self.ptr: z.do('writer.write_array(%s->uint32, 4);' % varname)
			else: z.do('writer.write_array(%s.uint32, 4);' % varname)
		elif self.type == 'VkClearValue': # union, requires special handling - _one_ case with a pointer array
			if not self.length:
				z.do('writer.write_array(%s.color.uint32, 4);' % varname)
			else:
				z.do('for (unsigned l = 0; l < %s; l++) writer.write_array(%s[l].color.uint32, 4);' % (owner + self.length, varname))
		elif self.string_array:
			z.do('writer.write_string_array(%s, %s);' % (varname, self.length.replace(',null-terminated', '')))
		elif self.string:
			z.do('writer.write_string(%s);' % varname)
		elif self.disphandle or self.nondisphandle: # writer handles (sic) this correctly through overloaded functions
			deref = '*' if self.ptr else ''
			if self.funcname in spec.functions_create and self.name == spec.functions_create[self.funcname][0]:
				assert not self.funcname in extra_optionals
				pass # handled elsewhere
			elif self.length:
				if 'VK_MAX_' not in self.length:
					z.do('for (unsigned hi = 0; hi < %s; hi++)' % (owner + self.length))
				else: # define variant
					z.do('for (unsigned hi = 0; hi < %s; hi++)' % self.length)
				z.loop_begin()
				z.do('auto* data = writer.parent->records.%s_index.at(%s[hi]);' % (self.type, varname))
				z.do('writer.write_handle(data);')
				if (self.funcname, self.name) in spec.externally_synchronized:
					z.do('data->tid = writer.thread_index();')
					z.do('data->call = writer.local_call_number;')

				if self.funcname in [ 'vkCmdBindVertexBuffers2', 'vkCmdBindVertexBuffers2EXT' ] and self.name == 'pBuffers':
					z.do('if (pBuffers[hi]) commandbuffer_data->touch(data, pOffsets[hi], pSizes ? pSizes[hi] : (data->size - pOffsets[hi]), __LINE__);') # TBD handle pStrides
				if self.funcname in [ 'vkCmdBindVertexBuffers' ] and self.name == 'pBuffers':
					z.do('if (pBuffers[hi]) commandbuffer_data->touch(data, pOffsets[hi], data->size - pOffsets[hi], __LINE__);')

				z.loop_end()
			else:
				z.decl(trackable_type_map_trace.get(self.type, 'trackable') + '*', totrackable(self.type))
				z.do('%s = writer.parent->records.%s_index.at(%s%s);' % (totrackable(self.type), self.type, deref, varname))
				if self.funcname in extra_optionals and self.name in extra_optionals[self.funcname]:
					z.do('if (%s)' % extra_optionals[self.funcname][self.name])
					z.brace_begin()
				z.do('writer.write_handle(%s);' % totrackable(self.type))

				if self.funcname in [ 'vkCmdBindIndexBuffer' ] and self.name == 'buffer':
					z.do('commandbuffer_data->indexBuffer.offset = offset;')
					z.do('commandbuffer_data->indexBuffer.buffer_data = buffer_data;')
					z.do('commandbuffer_data->indexBuffer.indexType = indexType;')
				elif self.funcname in [ 'vkCmdDispatchIndirect' ] and self.name == 'buffer':
					z.do('commandbuffer_data->touch(buffer_data, offset, sizeof(%s), __LINE__);' % spec.indirect_command_c_struct_names[self.funcname])
				elif self.funcname in [ 'vkCmdDrawIndirect', 'vkCmdDrawIndexedIndirect', 'vkCmdDrawMeshTasksIndirectEXT' ] and self.name == 'buffer':
					z.do('if (drawCount == 1) commandbuffer_data->touch(buffer_data, offset, sizeof(%s), __LINE__);' % spec.indirect_command_c_struct_names[self.funcname])
					z.do('else if (drawCount > 1) commandbuffer_data->touch(buffer_data, offset, stride * drawCount, __LINE__);')
					if 'Indexed' in name: z.do('commandbuffer_data->touch_index_buffer(0, VK_WHOLE_SIZE); // must check whole buffer here since we do not know yet what will be used')
				elif self.funcname in [ 'vkCmdDrawIndirectCount', 'vkCmdDrawIndirectCountKHR', 'vkCmdDrawIndirectCountAMD' ] and self.name == 'countBuffer':
					z.do('commandbuffer_data->touch(buffer_data, countBufferOffset, 4 * maxDrawCount, __LINE__);')
				elif self.funcname in [ 'vkCmdDrawIndirectCount', 'vkCmdDrawIndirectCountKHR', 'vkCmdDrawIndexedIndirectCount', 'vkCmdDrawIndexedIndirectCountKHR',
				                        'vkCmdDrawMeshTasksIndirectCountEXT', 'vkCmdDrawIndexedIndirectCount' ] and self.name == 'buffer':
					z.do('if (maxDrawCount > 0) commandbuffer_data->touch(buffer_data, offset, stride * maxDrawCount, __LINE__);')
					if 'Indexed' in name: z.do('commandbuffer_data->touch_index_buffer(0, VK_WHOLE_SIZE); // must check whole buffer here since we do not know yet what will be used')
				elif self.funcname in [ 'vkCmdCopyBuffer', 'VkCopyBufferInfo2', 'VkCopyBufferInfo2KHR' ] and self.name == 'srcBuffer':
					if self.funcname[0] == 'V': z.decl(trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					prefix = 'sptr->' if self.funcname[0] == 'V' else ''
					z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].srcOffset, %spRegions[ii].size, __LINE__);' % (prefix, prefix, prefix))
				elif self.funcname in [ 'vkCmdCopyBuffer', 'VkCopyBufferInfo2', 'VkCopyBufferInfo2KHR' ] and self.name == 'dstBuffer':
					if self.funcname[0] == 'V': z.decl(trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					prefix = 'sptr->' if self.funcname[0] == 'V' else ''
					z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].dstOffset, %spRegions[ii].size, __LINE__);' % (prefix, prefix, prefix))
				elif self.funcname in [ 'vkCmdCopyImage', 'vkCmdBlitImage', 'vkCmdCopyBufferToImage', 'vkCmdCopyImageToBuffer', 'vkCmdResolveImage', 'VkCopyImageInfo2',
							'VkCopyImageInfo2KHR', 'VkCopyBufferToImageInfo2', 'VkCopyBufferToImageInfo2KHR', 'VkCopyImageToBufferInfo2', 'VkCopyImageToBufferInfo2KHR',
							'VkBlitImageInfo2', 'VkBlitImageInfo2KHR', 'VkResolveImageInfo2', 'VkResolveImageInfo2KHR' ] and self.type == 'VkImage' and 'src' in self.name:
					if self.funcname[0] == 'V': z.decl(trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);') # TBD can calculate smaller area for some images but maybe not worth it
				elif self.funcname in [ 'vkCmdExecuteCommands' ]:
					z.do('for (unsigned ii = 0; ii < commandBufferCount; ii++) // copy over touched ranges from secondary to primary')
					z.loop_begin()
					z.do('const auto* other_cmdbuf_data = writer.parent->records.VkCommandBuffer_index.at(pCommandBuffers[ii]);')
					z.do('commandbuffer_data->touch_merge(other_cmdbuf_data->touched);')
					z.loop_end()
				elif self.funcname in [ 'vkCmdBeginRendering' ]:
					z.do('for (unsigned ii = 0; ii < pRenderingInfo->colorAttachmentCount; ii++)')
					z.loop_begin()
					z.do('if (pRenderingInfo->pColorAttachments[ii].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)')
					z.brace_begin()
					z.do('auto* imageview_data = writer.parent->records.VkImageView_index.at(pRenderingInfo->pColorAttachments[ii].imageView);')
					z.do('auto* image_data = writer.parent->records.VkImage_index.at(imageview_data->image);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);')
					z.brace_end()
					z.loop_end()
				elif self.funcname in [ 'vkCmdBeginRenderPass', 'vkCmdBeginRenderPass2', 'vkCmdBeginRenderPass2KHR' ]:
					z.do('auto* renderpass_data = writer.parent->records.VkRenderPass_index.at(pRenderPassBegin->renderPass);')
					z.do('auto* framebuffer_data = writer.parent->records.VkFramebuffer_index.at(pRenderPassBegin->framebuffer);')
					z.do('for (unsigned ii = 0; ii < framebuffer_data->imageviews.size() && renderpass_data; ii++)')
					z.loop_begin()
					z.do('if (renderpass_data->attachments[ii].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || renderpass_data->attachments[ii].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)')
					z.brace_begin()
					z.do('auto* image_data = writer.parent->records.VkImage_index.at(framebuffer_data->imageviews.at(ii)->image);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);')
					z.brace_end()
					z.loop_end()

				if (self.funcname, self.name) in spec.externally_synchronized or (self.funcname in spec.externally_synchronized_members and self.name in spec.externally_synchronized_members[self.funcname]):
					z.do('if (%s)' % totrackable(self.type))
					z.brace_begin()
					z.do('%s->tid = writer.thread_index();' % totrackable(self.type))
					z.do('%s->call = writer.local_call_number;' % totrackable(self.type))
					z.brace_end()
				if self.funcname in extra_optionals and self.name in extra_optionals[self.funcname]:
					z.brace_end()
		elif self.ptr and self.length:
			if self.type != 'void' and self.type in spec.type_mappings: # do we need to convert it?
				z.do('for (unsigned s = 0; s < (unsigned)(%s); s++) writer.write_%s(%s[s]);' % (self.length, spec.type_mappings[self.type], varname))
			else: # no, just write out as is
				z.do('writer.write_array(reinterpret_cast<const char*>(%s), %s * sizeof(%s));' % (varname, self.length, self.type if self.type != 'void' else 'char'))
		elif self.ptr and self.type in spec.type_mappings:
			z.do('writer.write_%s(*%s);' % (spec.type_mappings[self.type], varname))
		elif self.ptr:
			z.do('writer.write_%s(*%s);' % (self.type, varname))
		elif self.type in spec.type_mappings and self.length: # type mapped array
			z.do('writer.write_array(reinterpret_cast<%s%s*>(%s), %s);' % (self.mod, spec.type_mappings[self.type], varname, self.length))
		elif self.type in spec.type_mappings:
			z.do('writer.write_%s(%s);' % (spec.type_mappings[self.type], varname))
		elif self.ptr and self.length: # arrays
			z.do('writer.write_array(%s, %s);' % (varname, self.length))
		elif not self.ptr and self.length: # specific size arrays
			z.do('writer.write_array(%s, %s);' % (varname, self.length))
		else: # directly supported type
			z.do('writer.write_%s(%s);' % (self.type, varname))

		if self.name == 'sType':
			orig = z.struct_last()
			stype = spec.type2sType[orig]
			z.do('assert(%s == %s);' % (varname, stype))

		if self.optional and not self.name in skip_opt_check:
			z.brace_end()

		if self.funcname == 'vkEndCommandBuffer':
			z.do('commandbuffer_data->tid = writer.thread_index();')
			z.do('commandbuffer_data->call = writer.local_call_number;')
		if self.funcname == 'VkDebugMarkerObjectNameInfoEXT' and self.name == 'pObjectName':
			z.do('object_data->name = sptr->pObjectName;')
		if self.funcname == 'VkDebugUtilsObjectNameInfoEXT' and self.name == 'pObjectName':
			z.do('object_data->name = sptr->pObjectName;')
		if 'vkCmd' in self.funcname and self.type == 'VkCommandBuffer' and self.name == 'commandBuffer':
			z.do('writer.commandBuffer = %s;' % varname) # always earlier in the parameter list than images and buffers, fortunately
		if self.funcname in ['vkBindImageMemory', 'vkBindBufferMemory', 'VkBindImageMemoryInfo', 'VkBindImageMemoryInfoKHR', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR'] and self.name in ['image', 'buffer']:
			z.do('const auto* meminfo = writer.parent->records.VkDeviceMemory_index.at(%s);' % (owner + 'memory'))
			z.do('writer.write_uint32_t(static_cast<uint32_t>(meminfo->propertyFlags)); // save memory flags')
		if self.funcname in ['vkBindImageMemory', 'VkBindImageMemoryInfo', 'VkBindImageMemoryInfoKHR'] and self.name == 'image':
			z.do('const auto* imageinfo = writer.parent->records.VkImage_index.at(%s);' % varname)
			z.do('writer.write_uint32_t(static_cast<uint32_t>(imageinfo->tiling)); // save tiling info')
			z.do('writer.write_uint64_t(static_cast<uint64_t>(imageinfo->size)); // save padded image size')
		if self.funcname == 'vkAllocateMemory' and self.name == 'pAllocateInfo':
			z.do('frame_mutex.lock();')
			z.do('assert(real_memory_properties.memoryTypeCount > 0);')
			z.do('assert(virtual_memory_properties.memoryTypeCount > pAllocateInfo_ORIGINAL->memoryTypeIndex);')
			z.do('pAllocateInfo->memoryTypeIndex = remap_memory_types_to_real[pAllocateInfo->memoryTypeIndex]; // remap memory index')
			z.do('assert(real_memory_properties.memoryTypeCount > pAllocateInfo->memoryTypeIndex);')
			z.do('char* extmem = nullptr;')
			z.do('if (p__external_memory == 1 && (virtual_memory_properties.memoryTypes[pAllocateInfo_ORIGINAL->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))')
			z.brace_begin()
			z.do('if (find_extension(pAllocateInfo, VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT)) ABORT("Cannot replace host memory allocations - application is already doing it!");')
			z.do('VkImportMemoryHostPointerInfoEXT* info = writer.pool.allocate<VkImportMemoryHostPointerInfoEXT>(1);')
			z.do('info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;')
			z.do('info->pNext = pAllocateInfo->pNext;')
			z.do('info->handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT ;')
			z.do('info->pHostPointer = nullptr;')
			z.do('uint32_t alignment = std::max<uint32_t>(writer.parent->meta.external_memory.minImportedHostPointerAlignment, getpagesize());')
			z.do('uint64_t size = pAllocateInfo->allocationSize + alignment - 1 - (pAllocateInfo->allocationSize + alignment - 1) % alignment;')
			z.do('writer.parent->mem_wasted += size - pAllocateInfo->allocationSize;')
			z.do('writer.parent->mem_allocated += size;')
			z.do('if (posix_memalign((void**)&extmem, alignment, size) != 0) ABORT("Failed to allocate external memory");')
			z.do('info->pHostPointer = (void*)extmem;')
			z.do('pAllocateInfo->pNext = info;')
			z.brace_end()
			z.do('else writer.parent->mem_allocated += pAllocateInfo->allocationSize;')
			z.do('frame_mutex.unlock();')
		if self.funcname == 'VkMemoryUnmapInfoKHR' and self.name == 'memory':
			z.do('devicememory_data->ptr = nullptr;')
			z.do('devicememory_data->offset = 0;')
			z.do('devicememory_data->size = 0;')

		if debugcount >= 0 and self.funcname != 'vkDestroyInstance':
			z.do('writer.write_uint16_t(%d); // sentinel for %s' % (debugcount, varname))
			debugcount += 1

def get_create_params(name):
	count = spec.functions_create[name][1]
	if name in ['vkAllocateCommandBuffers', 'vkAllocateDescriptorSets']: # work around stupid design in XML
		count = 'pAllocateInfo->%s' % count
	return (spec.functions_create[name][0], count, spec.functions_create[name][2])

def save_add_pre(name): # need to include the resource-creating or resource-destroying command to avoid a race-condition
	z = getspool()
	if name in ['vkGetFenceStatus', 'vkWaitForFences']:
		# if waitAll=False for vkWaitForFences not all fences are signaled, but we assume it doesn't matter which one so we set all here anyways
		if name == 'vkGetFenceStatus': z.do('const uint32_t fenceCount = 1;')
		z.do('const int frame = lava_writer::instance().global_frame;')
		z.do('if (p__delay_fence_success_frames > 0) for (unsigned i = 0; i < fenceCount; i++)')
		z.brace_begin() # the assumption here is that the function is only called once per frame per fence unless it is in a loop
		z.do('auto* tf = writer.parent->records.VkFence_index.at(%s);' % ('pFences[i]' if name == 'vkWaitForFences' else 'fence'))
		z.do('if (tf->frame_delay == -1) { tf->frame_delay = p__delay_fence_success_frames - 1; }')
		if name == 'vkGetFenceStatus':
			z.do('if (tf->frame_delay >= 0) { tf->frame_delay--; writer.write_uint32_t(VK_NOT_READY); return VK_NOT_READY; }')
		elif name == 'vkWaitForFences':
			z.do('if (tf->frame_delay >= 0 && timeout != UINT32_MAX) { tf->frame_delay--; writer.write_uint32_t(VK_TIMEOUT); return VK_TIMEOUT; }')
		z.brace_end()
	elif name in ['vkUnmapMemory', 'vkUnmapMemory2KHR']:
		z.do('writer.parent->memory_mutex.lock();')

	if name == 'vkCreateSwapchainKHR': # TBD: also do vkCreateSharedSwapchainsKHR
		z.init('pCreateInfo->minImageCount = num_swapchains();')

def save_add_tracking(name):
	z = getspool()

	if name == 'vkCmdDrawMultiIndexedEXT':
		z.do('for (unsigned ii = 0; ii < drawCount; ii++)')
		z.loop_begin()
		z.do('commandbuffer_data->touch_index_buffer(pIndexInfo[ii].firstIndex, pIndexInfo[ii].indexCount);')
		z.loop_end()
	elif name in ['vkUnmapMemory', 'vkUnmapMemory2KHR']:
		if name == 'vkUnmapMemory':
			z.do('devicememory_data->ptr = nullptr;')
			z.do('devicememory_data->offset = 0;')
			z.do('devicememory_data->size = 0;')
		z.do('writer.parent->memory_mutex.unlock();')
	elif 'vkCmdDraw' in name and 'Indexed' in name and not 'Indirect' in name:
		z.do('commandbuffer_data->touch_index_buffer(firstIndex, indexCount);')
	elif name == 'vkResetCommandPool':
		z.do('if (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) commandpool_data->commandbuffers.clear();')
		z.do('else for (auto* i : commandpool_data->commandbuffers) i->touched.clear();')
	elif name == 'vkResetCommandBuffer' or name == 'vkBeginCommandBuffer':
		z.do('commandbuffer_data->tid = writer.thread_index();')
		z.do('commandbuffer_data->call = writer.local_call_number;')
		z.do('commandbuffer_data->touched.clear();')
	elif name == 'vkResetFences':
		z.do('for (unsigned i = 0; i < fenceCount; i++)')
		z.brace_begin()
		z.do('auto* tf = writer.parent->records.VkFence_index.at(pFences[i]);')
		z.do('tf->frame_delay = -1; // just to be sure')
		z.do('tf->tid = writer.thread_index();')
		z.do('tf->call = writer.local_call_number;')
		z.brace_end()
	elif name in [ 'vkCmdCopyBufferToImage', 'vkCmdCopyImageToBuffer', 'VkCopyBufferToImageInfo2', 'VkCopyBufferToImageInfo2KHR', 'VkCopyImageToBufferInfo2', 'VkCopyImageToBufferInfo2KHR' ]:
		if name[0] == 'V': z.decl(trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
		prefix = 'sptr->' if name[0] == 'V' else ''
		z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].bufferOffset, std::min(image_data->size, buffer_data->size - %spRegions[ii].bufferOffset), __LINE__);' % (prefix, prefix, prefix))
	elif name in spec.functions_create and spec.functions_create[name][1] == '1':
		(param, count, type) = get_create_params(name)
		z.do('auto* add = writer.parent->records.%s_index.add(*%s, lava_writer::instance().global_frame);' % (type, param))
		z.do('add->tid = writer.thread_index();')
		z.do('add->call = writer.local_call_number;')
		if type == 'VkBuffer':
			z.do('add->size = pCreateInfo->size;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->usage = pCreateInfo->usage;')
			z.do('add->sharingMode = pCreateInfo->sharingMode;')
			z.do('add->type = VK_OBJECT_TYPE_BUFFER;')
		elif type == 'VkImage':
			z.do('add->tiling = pCreateInfo->tiling;')
			z.do('add->usage = pCreateInfo->usage;')
			z.do('add->sharingMode = pCreateInfo->sharingMode;')
			z.do('add->imageType = pCreateInfo->imageType;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->format = pCreateInfo->format;')
			z.do('add->type = VK_OBJECT_TYPE_IMAGE;')
			z.do('if (pCreateInfo->flags & VK_IMAGE_CREATE_ALIAS_BIT) ELOG("Image aliasing detected! We need to implement support for this!");')
		elif type == 'VkRenderPass' and name == 'vkCreateRenderPass':
			z.do('add->attachments.resize(pCreateInfo->attachmentCount);')
			z.do('for (unsigned ii = 0; ii < pCreateInfo->attachmentCount; ii++) add->attachments[ii] = pCreateInfo->pAttachments[ii]; // struct copy')
			z.do('assert(!(pCreateInfo->flags & VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT)); // not supported yet')
		elif type == 'VkRenderPass' and name in ['vkCreateRenderPass2', 'vkCreateRenderPass2KHR']:
			z.do('add->attachments.resize(pCreateInfo->attachmentCount);')
			z.do('for (unsigned ii = 0; ii < pCreateInfo->attachmentCount; ii++)')
			z.brace_begin()
			z.do('add->attachments[ii].flags = pCreateInfo->pAttachments[ii].flags;')
			z.do('add->attachments[ii].format = pCreateInfo->pAttachments[ii].format;')
			z.do('add->attachments[ii].samples = pCreateInfo->pAttachments[ii].samples;')
			z.do('add->attachments[ii].loadOp = pCreateInfo->pAttachments[ii].loadOp;')
			z.do('add->attachments[ii].storeOp = pCreateInfo->pAttachments[ii].storeOp;')
			z.do('add->attachments[ii].stencilLoadOp = pCreateInfo->pAttachments[ii].stencilLoadOp;')
			z.do('add->attachments[ii].stencilStoreOp = pCreateInfo->pAttachments[ii].stencilStoreOp;')
			z.do('add->attachments[ii].initialLayout = pCreateInfo->pAttachments[ii].initialLayout;')
			z.do('add->attachments[ii].finalLayout = pCreateInfo->pAttachments[ii].finalLayout;')
			z.brace_end()
			z.do('assert(!(pCreateInfo->flags & VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT)); // not supported yet')
		elif type == 'VkFramebuffer':
			z.do('add->imageviews.resize(pCreateInfo->attachmentCount);')
			z.do('for (unsigned ii = 0; ii < pCreateInfo->attachmentCount; ii++)')
			z.loop_begin()
			z.do('auto* imageview_data = writer.parent->records.VkImageView_index.at(pCreateInfo->pAttachments[ii]);')
			z.do('add->imageviews[ii] = imageview_data;')
			z.loop_end()
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->width = pCreateInfo->width;')
			z.do('add->height = pCreateInfo->height;')
			z.do('add->layers = pCreateInfo->layers;')
		elif type == 'VkFence':
			z.do('add->flags = pCreateInfo->flags;')
		elif type == 'VkDeviceMemory':
			z.do('frame_mutex.lock();')
			z.do('add->propertyFlags = virtual_memory_properties.memoryTypes[pAllocateInfo_ORIGINAL->memoryTypeIndex].propertyFlags;')
			z.do('add->allocationSize = pAllocateInfo->allocationSize;')
			z.do('add->backing = *pMemory;')
			z.do('add->extmem = extmem;')
			z.do('frame_mutex.unlock();')
		elif type == 'VkImageView':
			z.do('add->image = pCreateInfo->image;')
			z.do('add->image_index = writer.parent->records.VkImage_index.at(pCreateInfo->image)->index;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->format = pCreateInfo->format;')
			z.do('add->components = pCreateInfo->components;')
			z.do('add->subresourceRange = pCreateInfo->subresourceRange;')
			z.do('add->viewType = pCreateInfo->viewType;')
		elif type == 'VkBufferView':
			z.do('add->buffer = pCreateInfo->buffer;')
			z.do('add->buffer_index = writer.parent->records.VkBuffer_index.at(pCreateInfo->buffer)->index;')
			z.do('add->format = pCreateInfo->format;')
			z.do('add->offset = pCreateInfo->offset;')
			z.do('add->range = pCreateInfo->range;')
			z.do('add->flags = pCreateInfo->flags;')
		elif type == 'VkSwapchainKHR':
			z.do('add->info = *pCreateInfo;')
		elif type == 'VkDevice':
			z.do('add->physicalDevice = physicalDevice;')
		z.do('DLOG2("insert %s into %s index %%u", (unsigned)add->index);' % (type, name))
		z.do('writer.write_handle(add);')
	elif name in spec.functions_create: # multiple
		(param, count, type) = get_create_params(name)
		z.do('for (unsigned i = 0; i < %s; i++)' % count)
		z.brace_begin()
		z.do('auto* add = writer.parent->records.%s_index.add(%s[i], lava_writer::instance().global_frame);' % (type, param))
		z.do('add->tid = writer.thread_index();')
		z.do('add->call = writer.local_call_number;')
		if type == 'VkCommandBuffer':
			z.do('add->pool = pAllocateInfo->commandPool;')
			z.do('auto* commandpool_data = writer.parent->records.VkCommandPool_index.at(pAllocateInfo->commandPool);')
			z.do('add->pool_index = commandpool_data->index;')
			z.do('add->level = pAllocateInfo->level;')
			z.do('commandpool_data->commandbuffers.insert(add);')
		elif type == 'VkDescriptorSet':
			z.do('add->pool = pAllocateInfo->descriptorPool;')
			z.do('add->pool_index = writer.parent->records.VkDescriptorPool_index.at(pAllocateInfo->descriptorPool)->index;')
		elif type == 'VkPipeline':
			z.do('add->flags = pCreateInfos[i].flags;')
			z.do('add->cache = pipelineCache;')
			if name == 'vkCreateGraphicsPipelines': z.do('add->type = VK_PIPELINE_BIND_POINT_GRAPHICS;')
			elif name == 'vkCreateComputePipelines': z.do('add->type = VK_PIPELINE_BIND_POINT_COMPUTE;')
		elif type == 'VkSwapchainKHR':
			z.do('add->info = pCreateInfos[i];')
		z.do('DLOG2("insert %s into %s index %%u", (unsigned)add->index);' % (type, name))
		z.do('writer.write_handle(add);')
		z.brace_end()
	elif name in spec.functions_destroy:
		param = spec.functions_destroy[name][0]
		count = spec.functions_destroy[name][1]
		type = spec.functions_destroy[name][2]
		if count == '1':
			z.do('if (%s != VK_NULL_HANDLE)' % param)
		else:
			z.do('for (unsigned i = 0; i < %s; i++)' % count)
		z.brace_begin()
		z.do('auto* meta = writer.parent->records.%s_index.unset(%s%s, lava_writer::instance().global_frame);' % (type, param, '' if count == '1' else '[i]'))
		z.do('DLOG2("removing %s from %s index %%u", (unsigned)meta->index);' % (type, name))
		z.do('meta->self_test();')
		if type == 'VkCommandBuffer':
			z.do('commandpool_data->commandbuffers.erase(meta);')
		z.brace_end()

# Run before execute
def load_add_pre(name):
	z = getspool()
	if name in spec.functions_create and spec.functions_create[name][1] == '1':
		(param, count, type) = get_create_params(name)
		z.decl(type, param)
		z.decl('uint32_t', toindex(type))
		z.do('%s = reader.read_handle();' % toindex(type))
	elif name in spec.functions_create: # multiple
		(param, count, type) = get_create_params(name)
		z.do('%s* %s = reader.pool.allocate<%s>(%s);' % (type, param, type, count))
		z.do('uint32_t* indices = reader.pool.allocate<uint32_t>(%s);' % count)
		z.do('for (unsigned i = 0; i < %s; i++)' % count)
		z.brace_begin()
		z.do('indices[i] = reader.read_handle();')
		z.brace_end()
	elif name == 'vkDestroyPipelineCache': # TBD autogenerate
		z.do('replay_pre_vkDestroyPipelineCache(reader, device, device_index, pipelineCache, pipelinecache_index);')

	if name == 'vkCreatePipelineCache': # TBD autogenerate
		z.do('replay_pre_vkCreatePipelineCache(reader, device, device_index, pCreateInfo, pipelinecache_index);')

# Run after execute
def load_add_tracking(name):
	z = getspool()
	if name in ignore_on_read:
		return
	if name in spec.functions_create:
		(param, count, type) = get_create_params(name)
		if count == '1':
			if type == 'VkSwapchainKHR':
				z.do('if (is_noscreen()) pSwapchain = (VkSwapchainKHR)((intptr_t)swapchainkhr_index + 1);')
			z.do('DLOG2("insert %s by %s index %%u", (unsigned)%s);' % (type, name, toindex(type)))
			z.do('if (%s) index_to_%s.set(%s, %s);' % (param, type, toindex(type), param))
			if type == 'VkSwapchainKHR':
				z.do('trackedswapchain_replay& data = VkSwapchainKHR_index.at(swapchainkhr_index);')
				z.do('data.info = *pCreateInfo; // struct copy')
				z.do('data.index = %s;' % toindex(type))
				z.do('data.device = device;')
			elif type == 'VkDevice':
				z.do('trackeddevice& device_data = VkDevice_index.at(device_index);')
				z.do('device_data.physicalDevice = physicalDevice; // track parentage')
		else: # multiple
			z.do('for (unsigned i = 0; i < %s; i++)' % count)
			z.brace_begin()
			if type == 'VkSwapchainKHR':
				z.do('if (is_noscreen()) pSwapchains[i] = (VkSwapchainKHR)((intptr_t)indices[i] + 1);')
			z.do('DLOG2("insert %s into %s index %%u at pos=%%u", indices[i], i);' % (type, name))
			z.do('if (%s[i]) index_to_%s.set(indices[i], %s[i]);' % (param, type, param))
			if type == 'VkSwapchainKHR':
				z.do('trackedswapchain_replay& data = VkSwapchainKHR_index.at(indices[i]);')
				z.do('data.info = pCreateInfos[i];')
				z.do('data.index = indices[i];')
			z.brace_end()
	elif name in spec.functions_destroy:
		param = spec.functions_destroy[name][0]
		count = spec.functions_destroy[name][1]
		type = spec.functions_destroy[name][2]
		if type == 'VkSwapchainKHR':
			z.do('trackedswapchain_replay& data = VkSwapchainKHR_index.at(swapchainkhr_index);')
			z.do('for (auto i : data.virtual_images) wrap_vkDestroyImage(device, i, nullptr);')
			z.do('if (data.virtual_cmdpool != VK_NULL_HANDLE) wrap_vkFreeCommandBuffers(device, data.virtual_cmdpool, data.virtual_cmdbuffers.size(), data.virtual_cmdbuffers.data());')
			z.do('wrap_vkDestroyCommandPool(device, data.virtual_cmdpool, nullptr);')
			z.do('wrap_vkDestroySemaphore(device, data.virtual_semaphore, nullptr);')
			z.do('for (auto i : data.virtual_fences) wrap_vkDestroyFence(device, i, nullptr);')

def func_common(name, node, read, target, header, guard_header=True):
	proto = node.find('proto')
	retval = proto.find('type').text
	if name in spec.protected_funcs:
		print >> target, '#ifdef %s // %s' % (spec.protected_funcs[name], name)
		if header and guard_header:
			print >> header, '#ifdef %s // %s' % (spec.protected_funcs[name], name)
		print >> target
	params = []
	for p in node.findall('param'):
		api = p.attrib.get('api')
		if api and api == "vulkansc": continue
		params.append(parameter(p, read=read, funcname=name))
	paramlist = [ x.parameter() for x in params ]
	return retval, params, paramlist

def func_common_end(name, target, header, add_dummy=False):
	z = getspool()
	if name in spec.protected_funcs:
		if name not in hardcoded and add_dummy and name not in functions_noop and name not in spec.disabled_functions:
			print >> target
			print >> target, '#else // %s' % spec.protected_funcs[name]
			print >> target
			print >> target, 'void retrace_%s(lava_file_reader& reader)' % name
			print >> target, '{'
			print >> target, '\tABORT("Attempt to call unimplemented protected function: %s");' % name
			print >> target, '}'

		print >> target
		print >> target, '#endif // %s 1' % spec.protected_funcs[name]
		print >> target
		if not add_dummy and header:
			if header: print >> header, '#endif // %s 2' % spec.protected_funcs[name]

def loadfunc(name, node, target, header):
	debugcount = isdebugcount()
	z = getspool()
	z.target(target)

	if debugcount >= 0: debugcount = 0

	retval, params, paramlist = func_common(name, node, read=True, target=target, header=header, guard_header=False)
	if name in spec.disabled or name in functions_noop or name in spec.disabled_functions or spec.str_contains_vendor(name):
		func_common_end(name, target=target, header=header, add_dummy=True)
		return
	if header: print >> header, 'void retrace_%s(lava_file_reader& reader);' % name
	if name in hardcoded or name in hardcoded_read:
		func_common_end(name, target=target, header=header, add_dummy=True)
		return
	print >> target, 'void retrace_%s(lava_file_reader& reader)' % name
	print >> target, '{'
	if debugstats:
		z.do('uint64_t startTime = gettime();')
	z.do('// -- Load --')
	for param in params:
		if param.inparam:
			param.print_load(param.name, '')
	if name in spec.special_count_funcs: # functions that work differently based on whether last param is a nullptr or not
		z.do('uint8_t do_call = reader.read_uint8_t();')
	z.do('// -- Execute --')
	for param in params:
		if not param.inparam and param.ptr and param.type != 'void' and not name in ignore_on_read and not name in spec.special_count_funcs and not name in spec.functions_create:
			vname = z.backing(param.type, param.name, size=param.length, struct=param.structure)
			z.do('%s = %s;' % (param.name, vname))
	load_add_pre(name)
	call_list = [ x.retrace_exec_param(name) for x in params ]
	if name in replay_pre_calls:
		z.do('replay_pre_%s(reader, %s);' % (name, ', '.join(call_list)))
	if debugstats:
		z.do('uint64_t apiTime = gettime();')
	if name in spec.special_count_funcs:
		if name in noscreen_calls:
			z.do('if (do_call == 1 && !is_noscreen())')
		else:
			z.do('if (do_call == 1)')
		z.brace_begin()
		parlist = []
		for vv in spec.special_count_funcs[name][2]:
			z.decl('std::vector<%s>' % vv[1], vv[0])
			parlist.append('nullptr')
			call_list = call_list[:-1]
		z.decl(spec.special_count_funcs[name][1] + '*', spec.special_count_funcs[name][0])
		z.do('%s = reader.pool.allocate<%s>(1);' % (spec.special_count_funcs[name][0], spec.special_count_funcs[name][1]))
		z.do('%swrap_%s(%s, %s);' % ('VkResult retval = ' if retval == 'VkResult' else '', name, ', '.join(call_list), ', '.join(parlist)))
		if retval == 'VkResult': z.do('assert(retval == VK_SUCCESS);');
		for vv in spec.special_count_funcs[name][2]:
			z.do('%s.resize(*%s);' % (vv[0], spec.special_count_funcs[name][0]))
			if vv[1] in spec.type2sType:
				z.do('for (auto& i : %s) { i.sType = %s; i.pNext = nullptr; }' % (vv[0], spec.type2sType[vv[1]]))
		parlist = []
		for vv in spec.special_count_funcs[name][2]:
			parlist.append(vv[0] + '.data()')
		z.do('%swrap_%s(%s, %s);' % ('retval = ' if retval == 'VkResult' else '', name, ', '.join(call_list), ', '.join(parlist)))
		if retval == 'VkResult':
			z.do('assert(retval == VK_SUCCESS);');
			z.do('(void)retval; // ignore return value');
		z.brace_end()
		if retval == 'VkResult':
			z.do('(void)reader.read_uint32_t(); // ignore stored return value')
		assert retval in ['VkResult', 'void'], 'Unhandled retval value'
	elif name == "vkCreateInstance":
		z.do('%s retval = vkuSetupInstance(%s);' % (retval, ', '.join(call_list)))
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('check_retval(stored_retval, retval);')
	elif name == "vkCreateDevice":
		z.do('%s retval = vkuSetupDevice(%s);' % (retval, ', '.join(call_list)))
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('check_retval(stored_retval, retval);')
	elif name == "vkGetFenceStatus": # loop until success to fix synchronization if originally successful
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_SUCCESS) { while ((retval = wrap_vkGetFenceStatus(device, fence)) != VK_SUCCESS) { usleep(1); }; }')
		z.do('else { retval = wrap_vkGetFenceStatus(device, fence); }')
	elif name == "vkWaitForFences": # as above
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_SUCCESS) { timeout = UINT64_MAX; }')
		z.do('else if (stored_retval == VK_TIMEOUT) { timeout = 0; }')
		z.do('retval = wrap_vkWaitForFences(device, fenceCount, pFences, waitAll, timeout);')
	elif name == "vkGetEventStatus": # loop until same result achieved
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_EVENT_SET || stored_retval == VK_EVENT_RESET) do { retval = wrap_vkGetEventStatus(device, event); } while (retval != stored_retval && retval != VK_ERROR_DEVICE_LOST);')
	elif name == 'vkAcquireNextImageKHR':
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_INCOMPLETE; // signal we skipped it')
		z.do('if (!is_noscreen() && (stored_retval == VK_SUCCESS || stored_retval == VK_SUBOPTIMAL_KHR))')
		z.brace_begin()
		z.do('retval = wrap_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, fence, pImageIndex); // overwriting the timeout')
		z.do('auto& %s = VkSwapchainKHR_index.at(swapchainkhr_index);' % totrackable('VkSwapchainKHR'))
		z.do('%s.next_swapchain_image = *pImageIndex; // do this before we overwrite this value with stored value from file below' % totrackable('VkSwapchainKHR'))
		z.brace_end()
		z.do('else cleanup_sync(index_to_VkQueue.at(0), 0, nullptr, 1, &semaphore, fence);') # just picking any queue here
	elif name == 'vkAcquireNextImage2KHR':
		z.do('const uint32_t %s = index_to_VkSwapchainKHR.index(pAcquireInfo->swapchain);' % toindex('VkSwapchainKHR'))
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_INCOMPLETE; // signal we skipped it')
		z.do('if (!is_noscreen() && (stored_retval == VK_SUCCESS || stored_retval == VK_SUBOPTIMAL_KHR))')
		z.brace_begin()
		z.do('pAcquireInfo->timeout = UINT64_MAX; // sucess in tracing needs success in replay')
		z.do('retval = wrap_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);')
		z.do('auto& %s = VkSwapchainKHR_index.at(%s);' % (totrackable('VkSwapchainKHR'), toindex('VkSwapchainKHR')))
		z.do('%s.next_swapchain_image = *pImageIndex;' % totrackable('VkSwapchainKHR')) # do this before we overwrite this value with stored value from file
		z.brace_end()
		z.do('else cleanup_sync(index_to_VkQueue.at(0), 0, nullptr, 1, &pAcquireInfo->semaphore, pAcquireInfo->fence);') # just picking any queue here
	elif name == 'vkQueuePresentKHR':
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('VkResult retval = VK_SUCCESS;')
		z.do('if (!is_noscreen())')
		z.brace_begin()
		z.do('retval = wrap_%s(%s);' % (name, ', '.join(call_list)))
		z.brace_end()
		z.do('else cleanup_sync(queue, pPresentInfo->waitSemaphoreCount, pPresentInfo->pWaitSemaphores, 0, nullptr, VK_NULL_HANDLE);')
	else:
		prefix = ''
		if name in layer_implemented:
			if name == 'vkSetDebugUtilsObjectNameEXT':
				prefix = 'if (wrap_%s && pNameInfo->objectType != VK_OBJECT_TYPE_PHYSICAL_DEVICE && pNameInfo->objectType != VK_OBJECT_TYPE_DEVICE_MEMORY) ' % name
			elif name == 'vkDebugMarkerSetObjectNameEXT':
				prefix = 'if (wrap_%s && pNameInfo->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_PHYSICAL_DEVICE_EXT && pNameInfo->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT) ' % name
			else:
				prefix = 'if (wrap_%s) ' % name
		elif name in noscreen_calls:
			prefix = 'if (!is_noscreen()) '
		if retval == 'void' and not name in ignore_on_read:
			z.do('%swrap_%s(%s);' % (prefix, name, ', '.join(call_list)))
		elif not name in ignore_on_read:
			if retval == 'VkResult':
				z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
			elif retval in ['uint64_t', 'uint32_t']:
				z.do('%s stored_retval = reader.read_%s();' % (retval, retval))
			elif retval == 'VkBool32':
				z.do('VkBool32 stored_retval = static_cast<VkBool32>(reader.read_uint32_t());')
			elif retval in ['VkDeviceAddress', 'VkDeviceSize']:
				z.do('(void)reader.read_uint64_t();')
			# if query succeeded in trace, make it succeed in replay (TBD: we need better fix here;
			# as this breaks dumping! yeah, it breaks if the app does this, too...)
			if name == 'vkGetQueryPoolResults':
				z.do('std::vector<char> data(dataSize);')
				call_list[5] = 'data.data()'
				z.do('if (stored_retval == VK_SUCCESS) { flags |= VK_QUERY_RESULT_WAIT_BIT; flags &= ~VK_QUERY_RESULT_PARTIAL_BIT; }')
			if name in layer_implemented or name in noscreen_calls:
				if retval == 'VkBool32': z.do('%s retval = VK_FALSE;' % retval)
				elif retval == 'VkResult': z.do('%s retval = VK_SUCCESS;' % retval)
				else: assert False, 'Unhandled return type %s from %s' % (retval, name)
				z.do('%sretval = wrap_%s(%s);' % (prefix, name, ', '.join(call_list)))
			else: z.do('%s retval = wrap_%s(%s);' % (retval, name, ', '.join(call_list)))
			if retval == 'VkBool32':
				z.do('assert(stored_retval == retval);')
			elif retval == 'VkResult' and name != 'vkQueuePresentKHR':
				z.do('check_retval(stored_retval, retval);')
			elif retval in ['uint64_t', 'uint32_t']:
				z.do('assert(stored_retval == retval);')
			elif retval in ['VkDeviceAddress', 'VkDeviceSize']:
				pass
			else: assert name == 'vkQueuePresentKHR', 'Unhandled return value type %s from %s' % (retval, name)
		else:
			if retval in ['VkResult', 'VkBool32']:
				z.do('// this function is ignored on replay')
				z.do('(void)reader.read_uint32_t(); // also ignore result return value')
	if debugstats:
		z.do('apiTime = gettime() - apiTime;')
	z.do('// -- Post --')
	if not name in spec.special_count_funcs and not name in skip_post_calls:
		for param in params:
			if not param.inparam:
				param.print_load(param.name, '')
	if name in ['vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR']:
		z.do('auto& %s = VkSwapchainKHR_index.at(%s);' % (totrackable('VkSwapchainKHR'), toindex('VkSwapchainKHR')))
		z.do('%s.next_stored_image = *pImageIndex;' % totrackable('VkSwapchainKHR'))
	load_add_tracking(name)
	if name in replay_post_calls: # hard-coded post handling
		if retval == 'void':
			z.do('replay_post_%s(reader, %s);' % (name, ', '.join(call_list)))
		else:
			z.do('replay_post_%s(reader, retval, %s);' % (name, ', '.join(call_list)))
	if debugstats:
		z.do('__atomic_add_fetch(&setup_time_%s, gettime() - startTime - apiTime, __ATOMIC_RELAXED);' % name)
		z.do('__atomic_add_fetch(&vulkan_time_%s, apiTime, __ATOMIC_RELAXED);' % name)
	z.dump()
	print >> target, '}'
	func_common_end(name, target=target, header=header, add_dummy=True)
	print >> target

def savefunc(name, node, target, header):
	debugcount = isdebugcount()
	z = getspool()
	z.target(target)

	if debugcount >= 0: debugcount = 0

	retval, params, paramlist = func_common(name, node, read=False, target=target, header=header)
	if name in spec.disabled or name in spec.disabled_functions or spec.str_contains_vendor(name):
		func_common_end(name, target=target, header=header)
		return
	print >> header, 'VKAPI_ATTR %s VKAPI_CALL trace_%s(%s);' % (retval, name, ', '.join(paramlist))
	if name in hardcoded or name in hardcoded_write:
		func_common_end(name, target=target, header=header)
		return

	for i in range(len(paramlist)):
		if name in deconstify and deconstify[name] in paramlist[i]:
			paramlist[i] = paramlist[i] + '_ORIGINAL'
			z.first('%s %s_impl = *%s_ORIGINAL; // struct copy, discarding the const' % (params[i].type, params[i].name, params[i].name))
			z.first('%s %s = &%s_impl;' % (params[i].type + '*', params[i].name, params[i].name))
	print >> target, 'VKAPI_ATTR %s VKAPI_CALL trace_%s(%s)' % (retval, name, ', '.join(paramlist))
	print >> target, '{'
	if name in functions_noop:
		print >> target, '\tassert(false);'
		z.dump()
		if retval != 'void':
			print >> target, '\treturn (%s)0;' % retval
		print >> target, '}\n'
		func_common_end(name, target=target, header=header)
		return
	if debugstats:
		z.do('uint64_t startTime = gettime();')
	call_list = [ x.trace_exec_param(name) for x in params ]
	z.declarations.insert(0, 'lava_file_writer& writer = write_header(\"%s\", %s%s);' % (name, name.upper(), ', true' if name in spec.functions_destroy or name in thread_barrier_funcs else ''))
	if name in trace_pre_calls: # this must be called before we start serializing our packet, as ...
		z.declarations.insert(0, 'trace_pre_%s(%s);' % (name, ', '.join(call_list))) # ... we may generate our own packets here
	for param in params:
		if param.inparam:
			param.print_save(param.name, '')
	if name in spec.special_count_funcs: # functions that work differently based on whether last param is a nullptr or not
		parlist = []
		for vv in spec.special_count_funcs[name][2]:
			parlist.append(vv[0])
		z.do('writer.write_uint8_t((%s) ? 1 : 0);' % ' && '.join(parlist))
	z.do('// -- Execute --')
	save_add_pre(name)
	if debugstats:
		z.do('uint64_t apiTime = gettime();')
	if name == "vkCreateInstance":
		z.do('#ifdef COMPILE_LAYER')
		z.do('%s retval = vkuSetupInstanceLayer(%s);' % (retval, ', '.join(call_list)))
		z.do('#else')
		z.do('%s retval = vkuSetupInstance(%s);' % (retval, ', '.join(call_list)))
		z.do('#endif')
	elif name == "vkCreateDevice":
		z.do('#ifdef COMPILE_LAYER')
		z.do('%s retval = vkuSetupDeviceLayer(%s);' % (retval, ', '.join(call_list)))
		z.do('#else')
		z.do('%s retval = vkuSetupDevice(%s);' % (retval, ', '.join(call_list)))
		z.do('#endif')
	elif name in ignore_on_trace:
		z.do('// native call skipped')
		if retval == 'VkResult':
			z.do('VkResult retval = VK_SUCCESS;')
		assert retval in ['void', 'VkResult'], 'Bad return type for ignore case'
	elif name in layer_implemented:
		if retval != 'void':
			z.do('%s retval = VK_SUCCESS;' % (retval))
			z.do('if (wrap_%s) retval = wrap_%s(%s);' % (name, name, ', '.join(call_list)))
		else:
			z.do('if (wrap_%s) wrap_%s(%s);' % (name, name, ', '.join(call_list)))
	else:
		if retval != 'void':
			z.do('%s retval = wrap_%s(%s);' % (retval, name, ', '.join(call_list)))
		else:
			z.do('wrap_%s(%s);' % (name, ', '.join(call_list)))
	if debugstats:
		z.do('apiTime = gettime() - apiTime;')
	z.do('// -- Post --')
	save_add_tracking(name)
	if retval == 'VkBool32' or retval == 'VkResult' or retval == 'uint32_t':
		z.do('writer.write_uint32_t(retval);')
	elif retval in ['VkDeviceAddress', 'uint64_t', 'VkDeviceSize']:
		z.do('writer.write_uint64_t(retval);')
	if not name in spec.special_count_funcs and not name in skip_post_calls:
		for param in params:
			if not param.inparam:
				param.print_save(param.name, '')

	# Push thread barriers to other threads for some functions
	if name in push_thread_barrier_funcs:
		if retval == 'VkResult':
			z.do('if (retval == VK_SUCCESS || retval == VK_SUBOPTIMAL_KHR) writer.push_thread_barriers();')
		else:
			z.do('writer.push_thread_barriers();')
	if debugstats:
		z.do('__atomic_add_fetch(&bytes_%s, writer.thaw(), __ATOMIC_RELAXED);' % name)
		z.do('__atomic_add_fetch(&calls_%s, 1, __ATOMIC_RELAXED);' % name)
		z.do('__atomic_add_fetch(&setup_time_%s, gettime() - startTime - apiTime, __ATOMIC_RELAXED);' % name)
		z.do('__atomic_add_fetch(&vulkan_time_%s, apiTime, __ATOMIC_RELAXED);' % name)
	if name in trace_post_calls: # hard-coded post handling, must be last
		if retval != 'void':
			z.do('trace_post_%s(writer, retval, %s);' % (name, ', '.join(call_list)))
		else:
			z.do('trace_post_%s(writer, %s);' % (name, ', '.join(call_list)))
	if name in feature_detection_funcs:
		z.do('writer.parent->usage_detection.check_%s(%s);' % (name, ', '.join(call_list)))
	if retval != 'void':
		z.do('// -- Return --')
		z.do('return retval;')
	z.dump()
	print >> target, '}'
	func_common_end(name, target=target, header=header)
	print >> target
