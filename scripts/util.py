#!/usr/bin/python3

import sys
sys.path.append('external/tracetooltests/scripts')
import xml.etree.ElementTree as ET
import re
import sys
import collections
import spec # from tracetooltests
spec.init()
import vkconfig as vk # local

def typetmpname(root):
	assert not '[' in root, 'Bad name %s' % root
	return 'tmp_' + root[0] + root.translate(root.maketrans('', '', '_*-.:<> '))

# Used to split output into declarations and instructions. This is needed because
# we need things declared inside narrower scopes to outlive those scopes, and it
# is nice for reusing temporaries (some functions would use a lot of them).
class spool(object):
	def __init__(self):
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
		if len(self.first_lines) > 0:
			print('\t// -- Re-declarations --', file=self.out)
		for v in self.first_lines:
			print('\t' + v, file=self.out)
		if len(self.declarations) > 0:
			print('\t// -- Declarations --', file=self.out)
		for v in self.declarations:
			print('\t' + v, file=self.out)
		if len(self.before_instr) > 0:
			print('\t// -- Initializations --', file=self.out)
		for v in self.before_instr:
			print('\t' + v, file=self.out)
		if len(self.instructions) > 0 and (len(self.before_instr) > 0 or len(self.declarations) > 0):
			print('\t// -- Instructions --', file=self.out)
		for v in self.instructions:
			print(v, file=self.out)
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

	def loop_end(self):
		self.brace_end()

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

class parameter(spec.base_parameter):
	def __init__(self, node, read, funcname, transitiveConst = False):
		super().__init__(node, read, funcname, transitiveConst)
		if funcname in vk.extra_optionals and self.name in vk.extra_optionals[funcname] and not self.string and not self.string_array:
			assert self.optional or self.disphandle or self.nondisphandle, '%s in %s is not marked as optional!' % (self.name, funcname)

	# Print a single parameter for a gfxr consumer
	def gfxr_parameter(self):
		name = 'gfxr_' + self.name
		if not self.ptr and (self.disphandle or self.nondisphandle):
			return ('%s%s%s%s' % (self.mod, 'format::HandleId', self.param_ptrstr, name)).strip()
		if self.disphandle or self.nondisphandle: # pointer to handle
			return ('%s%s%s' % ('HandlePointerDecoder<%s>' % self.type, self.param_ptrstr, name)).strip()
		if self.ptr:
			return ('%s%s%s' % ('StructPointerDecoder<Decoded_%s>' % self.type, self.param_ptrstr, name)).strip()
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
		side = ('reader' if self.read else 'writer')

		if mytype in vk.deconst_struct and not self.read and self.funcname[0] == 'V':
			z.do('%s* %s_impl = %s.pool.allocate<%s>(%s);' % (mytype, self.name, side, mytype, '1' if not size else size))

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

		if self.funcname[0] == 'V' and mytype in vk.deconst_struct and not self.read: # we cannot modify passed in memory when writing
			if size:
				z.do('%s_impl[sidx] = %s[sidx]; // struct copy, discarding the const' % (self.name, varname))
				z.do('%s_%s(%s, &%s_impl[sidx]);' % (('read' if self.read else 'write'), mytype, side, self.name))
			else:
				z.do('*%s_impl = *%s; // struct copy, discarding the const' % (self.name, varname))
				z.do('%s_%s(%s, %s_impl);' % (('read' if self.read else 'write'), mytype, side, self.name))
				z.do('%s = %s_impl; // replacing pointer' % (accessor, self.name))
		elif mytype in vk.deconst_struct and self.read and self.funcname[0] == 'V':
			z.do('%s_%s(%s, (%s*)%s);' % (('read' if self.read else 'write'), mytype, side, mytype, accessor))
		else:
			z.do('%s_%s(%s, %s);' % (('read' if self.read else 'write'), mytype, side, accessor))

		if size:
			z.loop_end()
			if self.funcname[0] == 'V' and mytype in vk.deconst_struct and not self.read:
				z.do('%s = %s_impl; // replacing pointer' % (varname, self.name))

	def print_load(self, name, owner): # called for each parameter
		global z

		varname = owner + name
		is_root = not isptr(varname)

		if not self.funcname in vk.noscreen_calls and not self.funcname in vk.virtualswap_calls and self.funcname[0] == 'v':
			assert self.type != 'VkSwapchainKHR', '%s has VkSwapchainKHR in %s' % (self.funcname, self.name)
		if not self.funcname in vk.noscreen_calls and self.funcname[0] == 'v':
			assert self.type != 'VkSurfaceKHR', '%s has VkSurfaceKHR in %s' % (self.funcname, self.name)

		if self.optional and not self.name in vk.skip_opt_check:
			z.do('%s = reader.read_uint8_t(); // whether we should load %s' % (z.tmp('uint8_t'), self.name))
			z.do('if (%s)' % z.tmp('uint8_t'))
			z.brace_begin()

		if self.name == 'pAllocator':
			z.decl('VkAllocationCallbacks', 'allocator', struct=True)
			z.decl('VkAllocationCallbacks*', 'pAllocator', custom='&allocator')
			z.do('allocators_set(pAllocator);')
		elif self.name in ['pUserData']:
			pass
		elif self.funcname in ['VkDebugMarkerObjectNameInfoEXT', 'VkDebugMarkerObjectTagInfoEXT', 'vkDebugReportMessageEXT'] and self.name == 'object':
			z.decl('uint64_t', 'object')
			z.do('%s = reader.read_handle(DEBUGPARAM("%s"));' % (varname, self.type))
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT', 'vkSetPrivateData', 'vkSetPrivateDataEXT', 'vkGetPrivateData', 'vkGetPrivateDataEXT'] and self.name == 'objectHandle':
			z.decl('uint64_t', 'objectHandle')
			z.do('%s = reader.read_handle(DEBUGPARAM("%s"));' % (varname, self.type))
		elif self.type in ['wl_display', 'wl_surface']:
			z.do('(void)reader.read_uint64_t();') # ignore it, if we actually replay on wayland, we'll have to regenerate it
		elif self.type == 'VkAccelerationStructureBuildRangeInfoKHR':
			assert(self.funcname == 'vkBuildAccelerationStructuresKHR' or self.funcname == 'vkCmdBuildAccelerationStructuresKHR')
			z.decl(self.type + '**', self.name)
			z.do('%s = reader.pool.allocate<VkAccelerationStructureBuildRangeInfoKHR*>(infoCount);' % varname)
			z.do('for (unsigned i = 0; i < infoCount; i++) %s[i] = reader.pool.allocate<VkAccelerationStructureBuildRangeInfoKHR>(pInfos[i].geometryCount);' % varname)
			z.do('for (unsigned i = 0; i < infoCount; i++) for (unsigned j = 0; j < pInfos[i].geometryCount; j++) { auto* p = %s[i]; read_VkAccelerationStructureBuildRangeInfoKHR(reader, &p[j]); }' % varname)
		elif self.name in ['deviceAddress', 'bufferDeviceAddress'] and self.type == 'VkDeviceAddress':
			z.decl('uint64_t', 'stored_address')
			z.do('stored_address = reader.read_uint64_t();')
			z.do('%s = reader.parent->device_address_remapping.translate_address(stored_address);' % varname)
			z.do('ILOG("%s changing device address from %%lu to %%lu", (unsigned long)stored_address, (unsigned long)%s);' % (self.funcname, varname))
		elif self.name == 'queueFamilyIndex':
			z.decl('uint32_t', self.name)
			z.do('%s = reader.read_uint32_t();' % self.name)
			z.do('if (%s == LAVATUBE_VIRTUAL_QUEUE) %s = selected_queue_family_index;' % (self.name, self.name))
			if not is_root: z.do('%s = %s;' % (varname, self.name))
		elif self.name == 'pHostPointer':
			if self.funcname in ['VkMemoryToImageCopy', 'VkImageToMemoryCopy']:
				tmp_uuint64t = z.tmp('uint64_t')
				z.do('%s = reader.read_uint64_t();' % tmp_uuint64t)
				z.do('if (%s > 0)' % tmp_uuint64t)
				z.brace_begin()
				tmp_uuint8t_ptr = z.tmpmem('uint8_t', tmp_uuint64t)
				z.do('reader.read_array(%s, %s);' % (tmp_uuint8t_ptr, tmp_uuint64t))
				z.do('%s = %s;' % (varname, tmp_uuint8t_ptr))
				z.brace_end()
				z.do('else %s = nullptr;' % varname)
			else:
				z.decl('%s%s%s' % (self.mod, self.type, self.param_ptrstr), self.name)
		elif (self.name == 'ppData' and self.funcname in ['vkMapMemory', 'vkMapMemory2KHR', 'vkMapMemory2']):
			z.decl('%s%s%s' % (self.mod, self.type, self.param_ptrstr), self.name)
		elif self.name == 'pfnUserCallback' and self.funcname == 'VkDebugUtilsMessengerCreateInfoEXT':
			z.do('%s = messenger_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pfnUserCallback' and self.funcname == 'VkDeviceDeviceMemoryReportCreateInfoEXT':
			z.do('%s = memory_report_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pfnCallback':
			z.do('%s = debug_report_callback; // hijacking this pointer with our own callback function' % varname)
		elif self.name == 'pNext':
			z.do('read_extension(reader, (VkBaseOutStructure**)&%s);' % varname)
		elif self.type == 'VkDescriptorDataEXT':
			z.decl('uint8_t', 'opt')
			z.do('switch (sptr->type)')
			z.brace_begin()
			z.do('case VK_DESCRIPTOR_TYPE_SAMPLER: { VkSampler* tmp = reader.pool.allocate<VkSampler>(1); uint32_t index = reader.read_handle(DEBUGPARAM("%s")); *tmp = index_to_VkSampler.at(index); sptr->data.pSampler = tmp; } break;' % self.type)
			z.do('case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorImageInfo>(1); sptr->data.pCombinedImageSampler = tmp; read_VkDescriptorImageInfo(reader, tmp); } else { sptr->data.pCombinedImageSampler = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorImageInfo>(1); sptr->data.pInputAttachmentImage = tmp; read_VkDescriptorImageInfo(reader, tmp); } else { sptr->data.pInputAttachmentImage = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorImageInfo>(1); sptr->data.pSampledImage = tmp; read_VkDescriptorImageInfo(reader, tmp); } else { sptr->data.pSampledImage = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorImageInfo>(1); sptr->data.pStorageImage = tmp; read_VkDescriptorImageInfo(reader, tmp); } else { sptr->data.pStorageImage = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorAddressInfoEXT>(1); sptr->data.pUniformTexelBuffer = tmp; read_VkDescriptorAddressInfoEXT(reader, tmp); } else { sptr->data.pUniformTexelBuffer = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorAddressInfoEXT>(1); sptr->data.pStorageTexelBuffer = tmp; read_VkDescriptorAddressInfoEXT(reader, tmp); } else { sptr->data.pStorageTexelBuffer = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorAddressInfoEXT>(1); sptr->data.pUniformBuffer = tmp; read_VkDescriptorAddressInfoEXT(reader, tmp); } else { sptr->data.pUniformBuffer = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: opt = reader.read_uint8_t(); if (opt) { auto* tmp = reader.pool.allocate<VkDescriptorAddressInfoEXT>(1); sptr->data.pStorageBuffer = tmp; read_VkDescriptorAddressInfoEXT(reader, tmp); } else { sptr->data.pStorageBuffer = nullptr; } break;')
			z.do('case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:')
			z.brace_begin()
			z.do('uint64_t stored_address = reader.read_uint64_t();')
			z.do('sptr->data.accelerationStructure = reader.parent->device_address_remapping.translate_address(stored_address);')
			z.do('ILOG("%s changing device address from %%lu to %%lu", (unsigned long)stored_address, (unsigned long)%s.accelerationStructure);' % (self.funcname, varname))
			z.brace_end()
			z.do('break;')
			z.do('default: ABORT("Unsupported descriptor type for VkDescriptorDataEXT"); break;')
			z.brace_end()
		elif self.string_array:
			len = self.length.split(',')[0]
			if self.funcname == 'VkInstanceCreateInfo' and self.name == 'ppEnabledExtensionNames':
				z.do('%s = instance_extensions(reader, sptr->%s);' % (varname, len))
			elif self.funcname == 'VkInstanceCreateInfo' and self.name == 'ppEnabledLayerNames':
				z.do('%s = instance_layers(reader, sptr->%s);' % (varname, len))
			elif self.funcname == 'VkDeviceCreateInfo' and self.name == 'ppEnabledExtensionNames':
				z.do('%s = device_extensions(sptr, reader, physicalDevice, sptr->%s);' % (varname, len))
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
				storedname = 'indices'
				z.decl('uint32_t*', storedname)
				z.do('%s = reader.pool.allocate<uint32_t>(%s);' % (storedname, self.length))
				z.do('#ifdef DEBUG')
				z.do('reader.read_handle_array("%s", %s, %s); // read unique indices to objects' % (self.type, storedname, self.length))
				z.do('#else')
				z.do('reader.read_handle_array(%s, %s); // read unique indices to objects' % (storedname, self.length))
				z.do('#endif')
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
				z.brace_end()
			elif self.ptr:
				tmpname = toindex(self.type)
				z.decl('uint32_t', tmpname)
				z.decl(self.type + '*', self.name)
				z.do('%s = reader.read_handle(DEBUGPARAM("%s"));' % (tmpname, self.type))
				z.do('*%s = index_to_%s.at(%s);' % (varname, self.type, tmpname))
			else:
				if not isptr(varname):
					z.decl(self.type, self.name)
				tmpname = toindex(self.type)
				z.decl('uint32_t', tmpname)
				z.do('%s = reader.read_handle(DEBUGPARAM("%s"));' % (tmpname, self.type))
				if self.type == 'VkPhysicalDevice':
					z.do('%s = selected_physical_device;' % varname)
				elif self.type != 'VkDeviceMemory' and not self.funcname in vk.ignore_on_read:
					z.do('%s = index_to_%s.at(%s);' % (varname, self.type, tmpname))
		elif self.type == 'VkDeviceOrHostAddressKHR' or self.type == 'VkDeviceOrHostAddressConstKHR':
			z.decl('uint64_t', 'stored_address')
			z.do('stored_address = reader.read_uint64_t();')
			z.do('%s.deviceAddress = reader.parent->device_address_remapping.translate_address(stored_address); // assume device address since we do not support host addresses' % varname)
			z.do('ILOG("%s changing device address from %%lu to %%lu", (unsigned long)stored_address, (unsigned long)%s.deviceAddress);' % (self.funcname, varname))
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
		elif self.type in spec.packed_bitfields:
			storedtype = spec.type_mappings[self.type]
			z.do('%s = (%s)reader.read_%s_t();' % (self.type, varname, storedtype))
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
				elif self.ptr: # but not root
					z.do('%s = reader.pool.allocate<%s>(1);' % (varname, self.type))
					z.do('*%s = static_cast<%s>(reader.read_%s());' % (varname, self.type, storedtype))
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
				z.access(self.name, self.name) # avoid a & being added
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
			elif not is_root and self.ptr:
				z.decl('%s%s' % (self.type, self.inline_ptrstr), self.name)
				z.do('%s = reader.pool.allocate<%s>(1);' % (self.name, self.type))
				z.do('*%s = reader.read_%s();' % (self.name, self.type))
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

		if self.funcname in ['vkBindImageMemory', 'vkBindBufferMemory', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo', 'VkBindTensorMemoryInfoARM']:
			if self.name in ['image', 'buffer', 'tensor']:
				if self.funcname in ['VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo', 'VkBindTensorMemoryInfoARM']:
					z.do('const uint32_t device_index = index_to_VkDevice.index(reader.device);')
				z.do('%s& %s = VkDevice_index.at(%s);' % (vk.trackable_type_map_replay['VkDevice'], totrackable('VkDevice'), toindex('VkDevice')))
				z.do('%s& %s = %s_index.at(%s);' % (vk.trackable_type_map_replay[self.type], totrackable(self.type), self.type, toindex(self.type)))
				z.do('%s.memory_flags = static_cast<VkMemoryPropertyFlags>(reader.read_uint32_t()); // fetch memory flags especially added' % totrackable(self.type)) # TBD remove
				if self.funcname in ['vkBindImageMemory', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo'] and self.name == 'image': # TBD remove me
					z.do('const VkImageTiling tiling = static_cast<VkImageTiling>(reader.read_uint32_t()); // fetch tiling property especially added')
					z.do('assert((lava_tiling)tiling == image_data.tiling);')
					z.do('const VkDeviceSize min_size = static_cast<VkDeviceSize>(reader.read_uint64_t()); // fetch padded memory size')
					z.do('assert(min_size == image_data.size);')
				z.do('memory_requirements reqs;')
				z.do('if (reader.run) reqs = get_%s_memory_requirements(reader.device, %s);' % (vk.trackable_type_map_replay[self.type], totrackable(self.type)))
				z.do('else reqs = get_fake_memory_requirements(reader.device, %s);' % totrackable(self.type))
				z.do('suballoc_location loc = device_data.allocator->add_trackedobject(reader.thread_index(), reqs, (uint64_t)%s, %s);' % (varname, totrackable(self.type)))
			if self.name == 'memory':
				z.do('assert(loc.memory != VK_NULL_HANDLE);')
				z.do('%s = loc.memory;' % varname) # relying on the order of arguments here; see case above
			elif self.name == 'memoryOffset':
				z.do('%s = loc.offset;' % varname) # relying on the order of arguments here; see case above

		if self.funcname in ['vkDestroyBuffer', 'vkDestroyImage', 'vkDestroyTensorARM', 'vkDestroyDevice', 'VkCreateDevice'] and self.name == 'device':  #['image', 'buffer', 'tensor', 'device']:
			z.do('%s& %s = %s_index.at(%s);' % (vk.trackable_type_map_replay['VkDevice'], totrackable('VkDevice'), self.type, toindex('VkDevice')))

		if self.funcname == 'vkDestroyBuffer' and self.name == 'buffer':
			z.do('if (buffer_index != CONTAINER_INVALID_INDEX) device_data.allocator->free_buffer(buffer_index);')
		elif self.funcname == 'vkDestroyImage' and self.name == 'image':
			z.do('device_data.allocator->free_image(image_index);')
		elif self.funcname == 'vkDestroyTensorARM' and self.name == 'tensor':
			z.do('device_data.allocator->free_tensor(tensorarm_index);')
		elif self.name == 'sType':
			orig = z.struct_last()
			stype = spec.type2sType[orig]
			z.do('assert(%s == %s);' % (varname, stype))
		elif self.funcname == 'vkDestroySurface' and self.name == 'surface':
			z.do('window_destroy(instance, surfacekhr_index);')
		elif self.funcname in ['VkDebugMarkerObjectNameInfoEXT', 'VkDebugMarkerObjectTagInfoEXT', 'vkDebugReportMessageEXT'] and self.name == 'object':
			z.do('%s = debug_object_lookup(%sobjectType, %s);' % (varname, owner, varname))
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT', 'vkSetPrivateData', 'vkSetPrivateDataEXT', 'vkGetPrivateData', 'vkGetPrivateDataEXT'] and self.name == 'objectHandle':
			z.do('%s = object_lookup(%sobjectType, %s);' % (varname, owner, varname))

		# Track our currently executing device
		if self.type == 'VkDevice' and self.funcname[0] == 'v' and self.name == 'device':
			z.do('reader.device = device;')
			z.do('reader.physicalDevice = VkDevice_index.at(device_index).physicalDevice;')
		if self.type == 'VkPhysicalDevice' and self.funcname[0] == 'v' and self.name == 'physicalDevice':
			z.do('reader.physicalDevice = physicalDevice;')
		if self.type == 'VkQueue' and self.funcname[0] == 'v' and self.name == 'queue':
			z.do('%s& queue_data = VkQueue_index.at(queue_index);' % vk.trackable_type_map_replay[self.type])
			z.do('reader.device = queue_data.device;')
			z.do('reader.physicalDevice = queue_data.physicalDevice;')
		if self.type == 'VkCommandBuffer' and self.name == 'commandBuffer' and self.funcname[0] == 'v':
			z.do('%s& commandbuffer_data = VkCommandBuffer_index.at(commandbuffer_index);' % vk.trackable_type_map_replay[self.type])
			z.do('reader.device = commandbuffer_data.device;')
			z.do('reader.physicalDevice = commandbuffer_data.physicalDevice;')

		if self.optional and not self.name in vk.skip_opt_check:
			z.brace_end()

	def print_save(self, name, owner, postprocess = False): # called for each parameter
		global z

		varname = owner + name
		is_root = not isptr(varname)

		if not is_root and iscount(self):
			z.decl('%s%s%s' % (self.mod if self.structure else '', self.type, self.param_ptrstr), self.name)
			z.do('%s = %s; // in case used elsewhere as a count' % (self.name, varname))

		if self.optional and not self.name in vk.skip_opt_check:
			z.decl('uint8_t', '%s_opt' % self.name)
			if self.funcname in vk.extra_optionals and self.name in vk.extra_optionals[self.funcname]:
				z.do('%s_opt = %s && %s;' % (self.name, vk.extra_optionals[self.funcname][self.name], varname))
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
		elif self.name == 'pHostPointer':
			if self.funcname in ['VkMemoryToImageCopy', 'VkImageToMemoryCopy']:
				z.decl('uint64_t', 'host_copy_size')
				z.do('host_copy_size = host_image_copy_size(writer.host_copy_format, &sptr->imageSubresource, &sptr->imageExtent, sptr->memoryRowLength, sptr->memoryImageHeight);')
				z.do('writer.write_uint64_t(host_copy_size);')
				z.do('if (host_copy_size > 0 && sptr->pHostPointer) writer.write_array(reinterpret_cast<const char*>(sptr->pHostPointer), host_copy_size);')
			else:
				pass
		elif (self.name == 'ppData' and self.funcname in ['vkMapMemory', 'vkMapMemory2KHR', 'vkMapMemory2']):
			pass
		elif self.structure:
			if self.name == 'pRegions' and self.type in ['VkMemoryToImageCopy', 'VkImageToMemoryCopy'] and self.funcname in ['VkCopyMemoryToImageInfo', 'VkCopyImageToMemoryInfo']:
				z.decl('VkFormat', 'prev_host_copy_format', custom='writer.host_copy_format')
				z.do('writer.host_copy_format = image_data ? image_data->format : VK_FORMAT_UNDEFINED;')
				self.print_struct(self.type, varname, owner, size=self.length)
				z.do('writer.host_copy_format = prev_host_copy_format;')
			else:
				self.print_struct(self.type, varname, owner, size=self.length)
		elif self.funcname in ['VkDebugMarkerObjectNameInfoEXT', 'VkDebugMarkerObjectTagInfoEXT', 'vkDebugReportMessageEXT'] and self.name == 'object':
			z.do('auto* object_data = debug_object_trackable(writer.parent->records, %sobjectType, %s);' % (owner, varname))
			z.do('writer.write_handle(object_data);')
		elif self.type == 'VkDescriptorDataEXT':
			z.do('switch (sptr->type)')
			z.brace_begin()
			z.do('case VK_DESCRIPTOR_TYPE_SAMPLER: { auto* sampler_data = writer.parent->records.VkSampler_index.at(*sptr->data.pSampler); writer.write_handle(sampler_data); } break;')
			z.do('case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: writer.write_uint8_t(sptr->data.pCombinedImageSampler != nullptr); if (sptr->data.pCombinedImageSampler) write_VkDescriptorImageInfo(writer, sptr->data.pCombinedImageSampler); break;')
			z.do('case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: writer.write_uint8_t(sptr->data.pInputAttachmentImage != nullptr); if (sptr->data.pInputAttachmentImage) write_VkDescriptorImageInfo(writer, sptr->data.pInputAttachmentImage); break;')
			z.do('case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: writer.write_uint8_t(sptr->data.pSampledImage != nullptr); if (sptr->data.pSampledImage) write_VkDescriptorImageInfo(writer, sptr->data.pSampledImage); break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: writer.write_uint8_t(sptr->data.pStorageImage != nullptr); if (sptr->data.pStorageImage) write_VkDescriptorImageInfo(writer, sptr->data.pStorageImage); break;')
			z.do('case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: writer.write_uint8_t(sptr->data.pUniformTexelBuffer != nullptr); if (sptr->data.pUniformTexelBuffer) write_VkDescriptorAddressInfoEXT(writer, sptr->data.pUniformTexelBuffer); break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: writer.write_uint8_t(sptr->data.pStorageTexelBuffer != nullptr); if (sptr->data.pStorageTexelBuffer) write_VkDescriptorAddressInfoEXT(writer, sptr->data.pStorageTexelBuffer); break;')
			z.do('case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: writer.write_uint8_t(sptr->data.pUniformBuffer != nullptr); if (sptr->data.pUniformBuffer) write_VkDescriptorAddressInfoEXT(writer, sptr->data.pUniformBuffer); break;')
			z.do('case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: writer.write_uint8_t(sptr->data.pStorageBuffer != nullptr); if (sptr->data.pStorageBuffer) write_VkDescriptorAddressInfoEXT(writer, sptr->data.pStorageBuffer); break;')
			z.do('case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: writer.write_uint64_t(sptr->data.accelerationStructure); break;')
			z.do('default: ABORT("Unsupported descriptor type for VkDescriptorDataEXT"); break;')
			z.brace_end()
		elif 'CaptureReplayHandle' in self.name:
			z.do('writer.write_uint64_t((uint64_t)%s);' % varname)
		elif self.funcname in ['VkDebugUtilsObjectNameInfoEXT', 'VkDebugUtilsObjectTagInfoEXT', 'vkSetPrivateData', 'vkSetPrivateDataEXT', 'vkGetPrivateData', 'vkGetPrivateDataEXT'] and self.name == 'objectHandle':
			z.do('auto* object_data = object_trackable(writer.parent->records, %sobjectType, %s);' % (owner, varname))
			z.do('writer.write_handle(object_data);')
		elif self.name == 'pNext':
			z.do('write_extension(writer, (VkBaseOutStructure*)%s);' % varname)
		elif self.type in ['wl_display', 'wl_surface']:
			z.do('writer.write_uint64_t((uint64_t)%s);' % varname) # write pointer value so that we can distinguish between different instances of it on replay, if we want to
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
				assert not self.funcname in vk.extra_optionals
				pass # handled elsewhere
			elif self.length:
				assert self.type != 'VkQueue', '%s has array queues' % self.funcname
				if 'VK_MAX_' not in self.length:
					z.do('for (unsigned hi = 0; hi < %s; hi++)' % (owner + self.length))
				else: # define variant
					z.do('for (unsigned hi = 0; hi < %s; hi++)' % self.length)
				z.loop_begin()
				z.do('auto* data = writer.parent->records.%s_index.at(%s[hi]);' % (self.type, varname))
				z.do('writer.write_handle(data);')
				if (self.funcname, self.name) in spec.externally_synchronized:
					z.do('data->last_modified = writer.current;')

				if self.funcname in [ 'vkCmdBindVertexBuffers2', 'vkCmdBindVertexBuffers2EXT' ] and self.name == 'pBuffers':
					z.do('if (pBuffers[hi]) commandbuffer_data->touch(data, pOffsets[hi], pSizes ? pSizes[hi] : (data->size - pOffsets[hi]), __LINE__);') # TBD handle pStrides
				if self.funcname in [ 'vkCmdBindVertexBuffers' ] and self.name == 'pBuffers':
					z.do('if (pBuffers[hi]) commandbuffer_data->touch(data, pOffsets[hi], data->size - pOffsets[hi], __LINE__);')

				z.loop_end()
			else:
				z.decl(vk.trackable_type_map_trace.get(self.type, 'trackable') + '*', totrackable(self.type))
				if self.type == 'VkQueue':
					assert not self.ptr, '%s has pointer queues' % self.funcname
					assert self.name == 'queue', '%s has queue not named queue' % self.funcname
					if not postprocess:
						z.declarations.insert(0, 'queue = (p__virtualqueues && queue != VK_NULL_HANDLE) ? ((trackedqueue*)queue)->realQueue : queue;')
						z.declarations.insert(0, 'VkQueue original_queue = queue;') # this goes first
						z.do('%s = (p__virtualqueues) ? ((trackedqueue*)original_queue) : writer.parent->records.VkQueue_index.at(queue);' % totrackable(self.type))
				else:
					z.do('%s = writer.parent->records.%s_index.at(%s%s);' % (totrackable(self.type), self.type, deref, varname))
				if self.funcname in vk.extra_optionals and self.name in vk.extra_optionals[self.funcname]:
					z.do('if (%s)' % vk.extra_optionals[self.funcname][self.name])
					z.brace_begin()
				z.do('if (%s) %s->self_test();' % (totrackable(self.type), totrackable(self.type)))
				z.do('writer.write_handle(%s);' % totrackable(self.type))

				if self.funcname in [ 'vkCmdBindIndexBuffer' ] and self.name == 'buffer':
					z.do('commandbuffer_data->indexBuffer.offset = offset;')
					z.do('commandbuffer_data->indexBuffer.buffer_data = buffer_data;')
					z.do('commandbuffer_data->indexBuffer.indexType = indexType;')
				elif self.funcname in [ 'vkCmdDispatchIndirect' ] and self.name == 'buffer' and not postprocess:
					z.do('commandbuffer_data->touch(buffer_data, offset, sizeof(%s), __LINE__);' % spec.indirect_command_c_struct_names[self.funcname])
				elif self.funcname in [ 'vkCmdDrawIndirect', 'vkCmdDrawIndexedIndirect', 'vkCmdDrawMeshTasksIndirectEXT' ] and self.name == 'buffer' and not postprocess:
					z.do('if (drawCount == 1) commandbuffer_data->touch(buffer_data, offset, sizeof(%s), __LINE__);' % spec.indirect_command_c_struct_names[self.funcname])
					z.do('else if (drawCount > 1) commandbuffer_data->touch(buffer_data, offset, stride * drawCount, __LINE__);')
					if 'Indexed' in name: z.do('commandbuffer_data->touch_index_buffer(0, VK_WHOLE_SIZE); // must check whole buffer here since we do not know yet what will be used')
				elif self.funcname in [ 'vkCmdDrawIndirectCount', 'vkCmdDrawIndirectCountKHR', 'vkCmdDrawIndirectCountAMD' ] and self.name == 'countBuffer' and not postprocess:
					z.do('commandbuffer_data->touch(buffer_data, countBufferOffset, 4 * maxDrawCount, __LINE__);')
				elif self.funcname in [ 'vkCmdDrawIndirectCount', 'vkCmdDrawIndirectCountKHR', 'vkCmdDrawIndexedIndirectCount', 'vkCmdDrawIndexedIndirectCountKHR',
				                        'vkCmdDrawMeshTasksIndirectCountEXT', 'vkCmdDrawIndexedIndirectCount' ] and self.name == 'buffer' and not postprocess:
					z.do('if (maxDrawCount > 0) commandbuffer_data->touch(buffer_data, offset, stride * maxDrawCount, __LINE__);')
					if 'Indexed' in name: z.do('commandbuffer_data->touch_index_buffer(0, VK_WHOLE_SIZE); // must check whole buffer here since we do not know yet what will be used')
				elif self.funcname in [ 'vkCmdCopyBuffer', 'VkCopyBufferInfo2', 'VkCopyBufferInfo2KHR' ] and self.name == 'srcBuffer' and not postprocess:
					if self.funcname[0] == 'V': z.decl(vk.trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					prefix = 'sptr->' if self.funcname[0] == 'V' else ''
					z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].srcOffset, %spRegions[ii].size, __LINE__);' % (prefix, prefix, prefix))
				elif self.funcname in [ 'vkCmdCopyBuffer', 'VkCopyBufferInfo2', 'VkCopyBufferInfo2KHR' ] and self.name == 'dstBuffer' and not postprocess:
					if self.funcname[0] == 'V': z.decl(vk.trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					prefix = 'sptr->' if self.funcname[0] == 'V' else ''
					z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].dstOffset, %spRegions[ii].size, __LINE__);' % (prefix, prefix, prefix))
				elif self.funcname in [ 'vkCmdCopyImage', 'vkCmdBlitImage', 'vkCmdCopyBufferToImage', 'vkCmdCopyImageToBuffer', 'vkCmdResolveImage', 'VkCopyImageInfo2',
							'VkCopyImageInfo2KHR', 'VkCopyBufferToImageInfo2', 'VkCopyBufferToImageInfo2KHR', 'VkCopyImageToBufferInfo2', 'VkCopyImageToBufferInfo2KHR',
							'VkBlitImageInfo2', 'VkBlitImageInfo2KHR', 'VkResolveImageInfo2', 'VkResolveImageInfo2KHR' ] and self.type == 'VkImage' and 'src' in self.name and not postprocess:
					if self.funcname[0] == 'V': z.decl(vk.trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);') # TBD can calculate smaller area for some images but maybe not worth it
				elif self.funcname in [ 'vkCmdExecuteCommands' ] and not postprocess:
					z.do('for (unsigned ii = 0; ii < commandBufferCount; ii++) // copy over touched ranges from secondary to primary')
					z.loop_begin()
					z.do('const auto* other_cmdbuf_data = writer.parent->records.VkCommandBuffer_index.at(pCommandBuffers[ii]);')
					z.do('commandbuffer_data->touch_merge(other_cmdbuf_data->touched);')
					z.loop_end()
				elif self.funcname == 'vkCmdPipelineBarrier' and not postprocess:
					z.do('for (unsigned ii = 0; ii < bufferMemoryBarrierCount; ii++)')
					z.loop_begin()
					z.do('auto* buffer_data = writer.parent->records.VkBuffer_index.at(pBufferMemoryBarriers[ii].buffer);')
					z.do('commandbuffer_data->touch(buffer_data, pBufferMemoryBarriers[ii].offset, pBufferMemoryBarriers[ii].size, __LINE__);')
					z.loop_end()
					z.do('for (unsigned ii = 0; ii < imageMemoryBarrierCount; ii++)')
					z.loop_begin()
					z.do('auto* image_data = writer.parent->records.VkImage_index.at(pImageMemoryBarriers->image);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);')
					z.loop_end()
				elif self.funcname in [ 'vkCmdBeginRendering', 'vkCmdBeginRenderingKHR' ] and not postprocess:
					z.do('for (unsigned ii = 0; ii < pRenderingInfo->colorAttachmentCount; ii++)')
					z.loop_begin()
					z.do('if (pRenderingInfo->pColorAttachments[ii].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)')
					z.brace_begin()
					z.do('auto* imageview_data = writer.parent->records.VkImageView_index.at(pRenderingInfo->pColorAttachments[ii].imageView);')
					z.do('auto* image_data = writer.parent->records.VkImage_index.at(imageview_data->image);')
					z.do('commandbuffer_data->touch(image_data, 0, image_data->size, __LINE__);')
					z.brace_end()
					z.loop_end()
				elif self.funcname in [ 'vkCmdBeginRenderPass', 'vkCmdBeginRenderPass2', 'vkCmdBeginRenderPass2KHR' ] and not postprocess:
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
					z.do('%s->last_modified = writer.current;' % totrackable(self.type))
					z.brace_end()
				if self.funcname in vk.extra_optionals and self.name in vk.extra_optionals[self.funcname]:
					z.brace_end()
		elif self.ptr and self.length:
			if self.type != 'void' and self.type in spec.type_mappings: # do we need to convert it?
				z.do('for (unsigned s = 0; s < (unsigned)(%s); s++) writer.write_%s(%s[s]);' % (self.length, spec.type_mappings[self.type], varname))
			else: # no, just write out as is
				z.do('writer.write_array(reinterpret_cast<const char*>(%s), %s * sizeof(%s));' % (varname, self.length, self.type if self.type != 'void' else 'char'))
		elif self.type in spec.packed_bitfields:
			storedtype = spec.type_mappings[self.type]
			z.do('writer.write_%s_t((%s)%s);' % (storedtype, storedtype, varname))
		elif self.ptr and self.type in spec.type_mappings:
			z.do('writer.write_%s(*%s);' % (spec.type_mappings[self.type], varname))
		elif self.ptr:
			z.do('writer.write_%s(*%s);' % (self.type, varname))
		elif self.type in spec.type_mappings and self.length: # type mapped array
			z.do('writer.write_array(reinterpret_cast<%s%s*>(%s), %s);' % (self.mod, spec.type_mappings[self.type], varname, self.length))
		elif self.name == 'queueFamilyIndex':
			if self.funcname[0] == 'V': z.do('trackedphysicaldevice* physicaldevice_data = writer.parent->records.VkPhysicalDevice_index.at(writer.physicalDevice);')
			z.do('const bool virtual_family = (physicaldevice_data->queueFamilyProperties.at(%s).queueFlags & VK_QUEUE_GRAPHICS_BIT) && p__virtualqueues;' % varname)
			z.do('writer.write_uint32_t(virtual_family ? LAVATUBE_VIRTUAL_QUEUE : %s);' % varname)
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

		if self.optional and not self.name in vk.skip_opt_check:
			z.brace_end()

		if self.funcname == 'vkEndCommandBuffer':
			z.do('commandbuffer_data->last_modified = writer.current;')
		if self.funcname == 'VkDebugMarkerObjectNameInfoEXT' and self.name == 'pObjectName':
			z.do('if (sptr->pObjectName) object_data->name = sptr->pObjectName;')
		if self.funcname == 'VkDebugUtilsObjectNameInfoEXT' and self.name == 'pObjectName':
			z.do('if (sptr->pObjectName) object_data->name = sptr->pObjectName;')
		if self.type == 'VkDevice' and self.funcname[0] == 'v' and self.name == 'device':
			z.do('writer.device = device;')
			z.do('writer.physicalDevice = device_data->physicalDevice;')
		if self.type == 'VkPhysicalDevice' and self.funcname[0] == 'v' and self.name == 'physicalDevice':
			z.do('writer.physicalDevice = physicalDevice;')
		if self.type == 'VkQueue' and self.funcname[0] == 'v' and self.name == 'queue':
			z.do('writer.device = queue_data->device;')
			z.do('writer.physicalDevice = queue_data->physicalDevice;')
		if self.type == 'VkCommandBuffer' and self.name == 'commandBuffer':
			z.do('writer.commandBuffer = %s;' % varname) # always earlier in the parameter list than images and buffers, fortunately
			z.do('writer.device = commandbuffer_data->device;')
			z.do('writer.physicalDevice = commandbuffer_data->physicalDevice;')
		if self.funcname in ['vkBindImageMemory', 'vkBindBufferMemory', 'VkBindImageMemoryInfo', 'VkBindImageMemoryInfoKHR', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR', 'VkBindTensorMemoryInfoARM'] and self.name in ['image', 'buffer', 'tensor']:
			z.do('const auto* meminfo = writer.parent->records.VkDeviceMemory_index.at(%s);' % (owner + 'memory'))
			z.do('%s->memory_flags = meminfo->propertyFlags;' % totrackable(self.type))
			z.do('writer.write_uint32_t(static_cast<uint32_t>(meminfo->propertyFlags)); // save memory flags') # TBD remove
		if self.funcname in ['vkBindImageMemory', 'VkBindImageMemoryInfo', 'VkBindImageMemoryInfoKHR'] and self.name == 'image': # TBD remove
			z.do('writer.write_uint32_t(static_cast<uint32_t>(image_data->tiling)); // save tiling info') # TBD remove
			z.do('writer.write_uint64_t(static_cast<uint64_t>(image_data->size)); // save padded image size') # TBD remove
		if self.funcname == 'vkAllocateMemory' and self.name == 'pAllocateInfo' and not postprocess:
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
		z.do('if (tf->frame_delay == -1) { tf->frame_delay = p__delay_fence_success_frames; }')
		if name == 'vkGetFenceStatus':
			z.do('if (tf->frame_delay > 0) { tf->frame_delay--; writer.write_uint32_t(VK_NOT_READY); return VK_NOT_READY; }')
		elif name == 'vkWaitForFences':
			z.do('if (tf->frame_delay > 0 && timeout != UINT32_MAX) { tf->frame_delay--; writer.write_uint32_t(VK_TIMEOUT); return VK_TIMEOUT; }')
		z.brace_end()
	elif name in ['vkUnmapMemory', 'vkUnmapMemory2KHR']:
		z.do('writer.parent->memory_mutex.lock();')

	if name == 'vkCreateSwapchainKHR': # TBD: also do vkCreateSharedSwapchainsKHR
		z.init('pCreateInfo->minImageCount = num_swapchains();')
	elif name == 'vkCreateDevice':
		z.declarations.insert(0, 'std::unordered_set<std::string> requested_device_extensions;')
		z.declarations.insert(1, 'for (unsigned i = 0; i < pCreateInfo->enabledExtensionCount; i++) requested_device_extensions.insert(pCreateInfo->ppEnabledExtensionNames[i]);')
		z.declarations.insert(2, 'bool explicit_host_updates = p__trust_host_flushes;')
		z.declarations.insert(3, 'VkPhysicalDeviceExplicitHostUpdatesFeaturesARM* pdehuf = (VkPhysicalDeviceExplicitHostUpdatesFeaturesARM*)find_extension(pCreateInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXPLICIT_HOST_UPDATES_FEATURES_ARM);')
		z.declarations.insert(4, 'if (requested_device_extensions.count(VK_ARM_EXPLICIT_HOST_UPDATES_EXTENSION_NAME) && pdehuf && pdehuf->explicitHostUpdates == VK_TRUE) explicit_host_updates = true;')

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
		z.do('commandbuffer_data->last_modified = writer.current;')
		z.do('commandbuffer_data->touched.clear();')
	elif name == 'vkResetFences':
		z.do('for (unsigned i = 0; i < fenceCount; i++)')
		z.brace_begin()
		z.do('auto* tf = writer.parent->records.VkFence_index.at(pFences[i]);')
		z.do('tf->frame_delay = -1; // reset delay fuse')
		z.do('tf->last_modified = writer.current;')
		z.brace_end()
	elif name in [ 'vkCmdCopyBufferToImage', 'vkCmdCopyImageToBuffer', 'VkCopyBufferToImageInfo2', 'VkCopyBufferToImageInfo2KHR', 'VkCopyImageToBufferInfo2', 'VkCopyImageToBufferInfo2KHR' ]:
		if name[0] == 'V': z.decl(vk.trackable_type_map_trace['VkCommandBuffer'] + '*', totrackable('VkCommandBuffer'), custom='writer.parent->records.VkCommandBuffer_index.at(writer.commandBuffer);')
		prefix = 'sptr->' if name[0] == 'V' else ''
		z.do('for (unsigned ii = 0; ii < %sregionCount; ii++) commandbuffer_data->touch(buffer_data, %spRegions[ii].bufferOffset, std::min(image_data->size, buffer_data->size - %spRegions[ii].bufferOffset), __LINE__);' % (prefix, prefix, prefix))
	elif name in spec.functions_create and spec.functions_create[name][1] == '1':
		(param, count, type) = get_create_params(name)
		z.do('if (retval == VK_SUCCESS)')
		z.brace_begin()
		z.do('auto* add = writer.parent->records.%s_index.add(*%s, writer.current);' % (type, param))
		if type == 'VkBuffer':
			z.do('add->parent_device_index = device_data->index;')
			z.do('add->size = pCreateInfo->size;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->usage = pCreateInfo->usage;')
			z.do('auto* usageflags2 = (VkBufferUsageFlags2CreateInfo*)find_extension(pCreateInfo, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO);')
			z.do('if (usageflags2) add->usage2 = usageflags2->usage;')
			z.do('add->sharingMode = pCreateInfo->sharingMode;')
			z.do('add->object_type = VK_OBJECT_TYPE_BUFFER;')
		elif type == 'VkPipelineLayout':
			z.do('for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++) { const auto& v = pCreateInfo->pPushConstantRanges[i]; if (add->push_constant_space_used < v.offset + v.size) add->push_constant_space_used = v.offset + v.size; }')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->layouts.reserve(pCreateInfo->setLayoutCount);')
			z.do('for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) add->layouts.push_back(pCreateInfo->pSetLayouts[i]);')
		elif type == 'VkImage':
			z.do('add->parent_device_index = device_data->index;')
			z.do('add->tiling = (lava_tiling)pCreateInfo->tiling;')
			z.do('add->usage = pCreateInfo->usage;')
			z.do('add->sharingMode = pCreateInfo->sharingMode;')
			z.do('add->imageType = pCreateInfo->imageType;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->format = pCreateInfo->format;')
			z.do('add->object_type = VK_OBJECT_TYPE_IMAGE;')
			z.do('add->initialLayout = pCreateInfo->initialLayout;')
			z.do('add->currentLayout = pCreateInfo->initialLayout;')
			z.do('add->samples = pCreateInfo->samples;')
			z.do('add->extent = pCreateInfo->extent;')
			z.do('add->mipLevels = pCreateInfo->mipLevels;')
			z.do('add->arrayLayers = pCreateInfo->arrayLayers;')
			z.do('if (pCreateInfo->flags & VK_IMAGE_CREATE_ALIAS_BIT) ELOG("Image aliasing detected! We need to implement support for this!");')
		elif type == 'VkShaderModule':
			z.do('add->size = pCreateInfo->codeSize;')
			z.do('add->device_index = device_data->index;');
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
			z.do('add->explicit_host_updates = explicit_host_updates;')
			z.do('add->requested_device_extensions = requested_device_extensions;')
		elif type == 'VkTensorARM':
			z.do('add->parent_device_index = device_data->index;')
			z.do('add->object_type = VK_OBJECT_TYPE_TENSOR_ARM;')
			z.do('add->flags = pCreateInfo->flags;')
			z.do('add->sharingMode = pCreateInfo->sharingMode;')
			z.do('add->tiling = (lava_tiling)pCreateInfo->pDescription->tiling;')
			z.do('add->format = pCreateInfo->pDescription->format;')
			z.do('add->usage = pCreateInfo->pDescription->usage;')
			z.do('add->dimensions.resize(pCreateInfo->pDescription->dimensionCount);')
			z.do('if (pCreateInfo->pDescription->pStrides) add->strides.resize(pCreateInfo->pDescription->dimensionCount);')
			z.do('memcpy(add->dimensions.data(), pCreateInfo->pDescription->pDimensions, sizeof(int64_t) * pCreateInfo->pDescription->dimensionCount);')
			z.do('if (pCreateInfo->pDescription->pStrides) memcpy(add->strides.data(), pCreateInfo->pDescription->pStrides, sizeof(int64_t) * pCreateInfo->pDescription->dimensionCount);')
		elif type == 'VkAccelerationStructureKHR':
			z.do('add->parent_device_index = device_data->index;')
			z.do('add->flags = pCreateInfo->createFlags;')
			z.do('add->type = pCreateInfo->type;')
			z.do('add->offset = pCreateInfo->offset;')
			z.do('add->buffer = pCreateInfo->buffer;')
			z.do('add->buffer_index = writer.parent->records.VkBuffer_index.at(pCreateInfo->buffer)->index;')
			z.do('add->size = pCreateInfo->size;')
			z.do('add->object_type = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;')
		z.do('DLOG2("insert %s into %s index %%u at call=%%d", (unsigned)add->index, (int)writer.current.call);' % (type, name))
		z.do('add->enter_created();')
		z.do('add->self_test();')
		z.do('writer.write_handle(add);')
		z.brace_end()
		z.do('else')
		z.brace_begin()
		z.do('writer.write_handle(nullptr);')
		z.brace_end()
	elif name in spec.functions_create: # multiple
		(param, count, type) = get_create_params(name)
		z.do('for (unsigned i = 0; i < %s; i++)' % count)
		z.brace_begin()
		z.do('if (retval != VK_SUCCESS) { writer.write_handle(nullptr); continue; }')
		z.do('auto* add = writer.parent->records.%s_index.add(%s[i], writer.current);' % (type, param))
		if type == 'VkCommandBuffer':
			z.do('add->pool = pAllocateInfo->commandPool;')
			z.do('add->device = device;')
			z.do('add->physicalDevice = writer.parent->records.VkDevice_index.at(device)->physicalDevice;')
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
			z.do('add->device_index = device_data->index;');
			if name == 'vkCreateGraphicsPipelines': z.do('add->type = VK_PIPELINE_BIND_POINT_GRAPHICS;')
			elif name == 'vkCreateComputePipelines': z.do('add->type = VK_PIPELINE_BIND_POINT_COMPUTE;')
			elif name == 'PFN_vkCreateRayTracingPipelinesKHR': z.do('add->type = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;')
		elif type == 'VkSwapchainKHR':
			z.do('add->info = pCreateInfos[i];')
		z.do('DLOG2("insert %s into %s index %%u call=%%d", (unsigned)add->index, (int)writer.current.call);' % (type, name))
		z.do('add->enter_created();')
		z.do('writer.write_handle(add);')
		z.brace_end()
	elif name in spec.functions_destroy:
		param = spec.functions_destroy[name][0]
		count = spec.functions_destroy[name][1]
		type = spec.functions_destroy[name][2]
		if count == '1':
			z.do('if (writer.run && %s != VK_NULL_HANDLE)' % param)
			z.brace_begin()
		else:
			z.do('for (unsigned i = 0; i < %s; i++)' % count)
			z.brace_begin()
			z.do('if (!writer.run || %s[i] == VK_NULL_HANDLE) continue;' % param)
		z.do('auto* meta = writer.parent->records.%s_index.unset(%s%s, writer.current);' % (type, param, '' if count == '1' else '[i]'))
		z.do('DLOG2("removing %s from %s index %%u", (unsigned)meta->index);' % (type, name))
		z.do('meta->destroyed = writer.current;')
		z.do('meta->enter_destroyed();')
		if type == 'VkCommandBuffer':
			z.do('commandpool_data->commandbuffers.erase(meta);')
		elif type in ['VkBuffer', 'VkImage', 'VkAccelerationStructureKHR', 'VkTensorARM']:
			z.do('if (meta->backing != VK_NULL_HANDLE)')
			z.brace_begin()
			z.do('writer.parent->memory_mutex.lock();')
			z.do('auto* memory_data = writer.parent->records.VkDeviceMemory_index.at(meta->backing);')
			z.do('if (memory_data) memory_data->unbind(meta);')
			z.do('writer.parent->memory_mutex.unlock();')
			z.brace_end()
		z.brace_end()
	elif name == 'vkGetDescriptorSetLayoutSizeEXT':
		type = 'VkDescriptorSetLayout'
		z.do('if (writer.run) descriptorsetlayout_data->size = *pLayoutSizeInBytes;')
	elif name == 'vkGetDescriptorSetLayoutBindingOffsetEXT':
		type = 'VkDescriptorSetLayout'
		z.do('if (writer.run) descriptorsetlayout_data->offsets[binding] = *pOffset;')

# Run before execute
def load_add_pre(name):
	z = getspool()
	if name in spec.functions_create and spec.functions_create[name][1] == '1':
		(param, count, type) = get_create_params(name)
		z.decl(type, param)
		z.decl('uint32_t', toindex(type))
		z.do('%s = reader.read_handle(DEBUGPARAM("%s"));' % (toindex(type), type))
	elif name in spec.functions_create: # multiple
		(param, count, type) = get_create_params(name)
		z.do('%s* %s = reader.pool.allocate<%s>(%s);' % (type, param, type, count))
		z.do('uint32_t* indices = reader.pool.allocate<uint32_t>(%s);' % count)
		z.do('for (unsigned i = 0; i < %s; i++)' % count)
		z.brace_begin()
		z.do('indices[i] = reader.read_handle(DEBUGPARAM("%s"));' % type)
		z.brace_end()
	elif name in spec.functions_destroy:
		param = spec.functions_destroy[name][0]
		count = spec.functions_destroy[name][1]
		type = spec.functions_destroy[name][2]
		if type == 'VkDevice':
			z.do('device_data.allocator->self_test();')
			z.do('device_data.allocator->destroy();')
			z.do('suballoc_metrics sm = device_data.allocator->performance();')
			z.do('ILOG("Suballocator used=%lu allocated=%lu heaps=%u objects=%u efficiency=%g", (unsigned long)sm.used, (unsigned long)sm.allocated, (unsigned)sm.heaps, (unsigned)sm.objects, sm.efficiency);')
			z.do('assert(device_data.allocator->self_test() == 0);')
			z.do('delete device_data.allocator;')
			z.do('device_data.allocator = nullptr;')

	if name == 'vkCreatePipelineCache': # cannot be autogenerated, need the index parameter
		z.do('if (reader.run) replay_pre_vkCreatePipelineCache(reader, device, device_index, pCreateInfo, pipelinecache_index);')

# Run after execute
def load_add_tracking(name):
	z = getspool()
	if name in vk.ignore_on_read:
		return
	if name in spec.functions_create:
		(param, count, type) = get_create_params(name)
		if count == '1':
			z.do('DLOG2("insert %s by %s index %%u call=%%d", (unsigned)%s, (int)reader.current.call);' % (type, name, toindex(type)))
			if type == 'VkSwapchainKHR':
				z.do('if (is_noscreen() || !reader.run) pSwapchain = fake_handle<VkSwapchainKHR>(swapchainkhr_index);')
			else:
				z.do('if (!reader.run) %s = fake_handle<%s>(%s);' % (param, type, toindex(type)))
			z.do('if (%s) index_to_%s.set(%s, %s);' % (param, type, toindex(type), param))
			z.do('auto& data = %s_index.at(%s);' % (type, toindex(type)))
			z.do('assert(data.creation.frame == reader.current.frame);')
			z.do('data.creation = reader.current;')
			z.do('data.last_modified = reader.current;')
			if type == 'VkSwapchainKHR':
				z.do('data.info = *pCreateInfo; // struct copy')
				z.do('data.index = %s;' % toindex(type))
				z.do('data.device = device;')
			elif type == 'VkDevice':
				z.do('data.physicalDevice = physicalDevice; // track parentage')
				z.do('data.allocator = new suballocator();')
				z.do('data.allocator->create(selected_physical_device, pDevice, VkImage_index, VkBuffer_index, VkTensorARM_index, reader.run);')
			elif type == 'VkImage':
				z.do('data.tiling = (lava_tiling)pCreateInfo->tiling;')
				z.do('data.usage = pCreateInfo->usage;')
				z.do('data.sharingMode = pCreateInfo->sharingMode;')
				z.do('data.imageType = pCreateInfo->imageType;')
				z.do('data.samples = pCreateInfo->samples;')
				z.do('data.initialLayout = pCreateInfo->initialLayout; // duplicates info stored in json but needed for compatibility with older traces')
				z.do('data.currentLayout = pCreateInfo->initialLayout;')
				z.do('data.format = pCreateInfo->format; // as above, might be missing in json')
				z.do('data.extent = pCreateInfo->extent; // also might be missing in json')
				z.do('data.mipLevels = pCreateInfo->mipLevels; // as above')
				z.do('data.arrayLayers = pCreateInfo->arrayLayers; // as above')
				z.do('assert(data.format == pCreateInfo->format);') # sanity check
			elif type == 'VkDescriptorSet':
				z.do('data.pool = pAllocateInfo->descriptorPool;')
			elif type == 'VkShaderModule':
				z.do('data.device_index = device_index;');
				z.do('data.size = pCreateInfo->codeSize;')
				z.do('data.code.resize(pCreateInfo->codeSize / sizeof(uint32_t));')
				z.do('memcpy(data.code.data(), pCreateInfo->pCode, pCreateInfo->codeSize);')
			z.do('data.enter_created();')
		else: # multiple
			z.do('for (unsigned i = 0; i < %s && retval == VK_SUCCESS; i++)' % count)
			z.brace_begin()
			z.do('DLOG2("insert %s into %s index %%u at pos=%%u call=%%d", indices[i], i, (int)reader.current.call);' % (type, name))
			z.do('auto& data = %s_index.at(indices[i]);' % type)
			z.do('assert(data.creation.frame == reader.current.frame);')
			z.do('data.creation = reader.current;')
			z.do('data.last_modified = reader.current;')
			if type == 'VkSwapchainKHR':
				z.do('if (is_noscreen() || !reader.run) pSwapchains[i] = fake_handle<VkSwapchainKHR>(indices[i]);')
			elif type == 'VkCommandBuffer':
				z.do('data.device = device;')
				z.do('data.physicalDevice = VkDevice_index.at(device_index).physicalDevice;')
				z.do('if (!reader.run) %s[i] = fake_handle<%s>(indices[i]);' % (param, type))
			elif type == 'VkPipeline':
				z.do('data.device_index = device_index;');
			else:
				z.do('if (!reader.run) %s[i] = fake_handle<%s>(indices[i]);' % (param, type))
			z.do('if (%s[i]) index_to_%s.set(indices[i], %s[i]);' % (param, type, param))
			if type == 'VkSwapchainKHR':
				z.do('data.info = pCreateInfos[i];')
				z.do('data.index = indices[i];')
			z.do('data.enter_created();')
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
		if count == '1' and name not in vk.ignore_on_read:
			z.do('auto& data = %s_index.at(%s);' % (type, toindex(type)))
			z.do('assert(data.destroyed.frame == UINT32_MAX || data.destroyed.frame == reader.current.frame);')
			z.do('data.destroyed = reader.current;')
			z.do('data.last_modified = reader.current;')
			z.do('data.enter_destroyed();')
			z.do('index_to_%s.unset(%s);' % (type, toindex(type)))
		elif name not in vk.ignore_on_read:
			z.do('if (indices[i] == CONTAINER_NULL_VALUE) continue;')
			z.do('auto& data = %s_index.at(indices[i]);' % type)
			z.do('assert(data.destroyed.frame == UINT32_MAX || data.destroyed.frame == reader.current.frame);')
			z.do('data.destroyed = reader.current;')
			z.do('data.last_modified = reader.current;')
			z.do('data.enter_destroyed();')
			z.do('index_to_%s.unset(indices[i]);' % type)
		z.brace_end()
	elif name == 'vkQueuePresentKHR':
		z.do('if (reader.new_frame()) // if it returns true, then we have hit the end of our global frame range, so terminate everything')
		z.brace_begin()
		z.do('if (reader.run) wrap_vkDeviceWaitIdle(queue_data.device);')
		z.do('reader.parent->finalize(true);')
		z.do('usleep(100); // hack to ensure all other, in-progress threads are completed or waiting forever before we destroy everything below')
		z.do('if (reader.run) terminate_all(reader, queue_data.device);')
		z.do('if (p__debug_destination) fclose(p__debug_destination);')
		z.do('return; // make sure we now do not run anything below this point')
		z.brace_end()
	elif name in ['vkBindImageMemory', 'VkBindImageMemoryInfoKHR', 'VkBindImageMemoryInfo']:
		z.do('image_data.backing = memory;')
		z.do('image_data.offset = memoryOffset;')
		z.do('image_data.size = loc.size;')
		z.do('image_data.enter_bound();')
	elif name in ['vkBindBufferMemory', 'VkBindBufferMemoryInfo', 'VkBindBufferMemoryInfoKHR']:
		z.do('buffer_data.backing = memory;')
		z.do('buffer_data.offset = memoryOffset;')
		z.do('buffer_data.enter_bound();')
	elif name == 'vkGetDescriptorSetLayoutSizeEXT':
		type = 'VkDescriptorSetLayout'
		z.do('auto& data = %s_index.at(%s);' % (type, toindex(type)))
		z.do('if (reader.run) data.size = *pLayoutSizeInBytes;')
	elif name == 'vkGetDescriptorSetLayoutBindingOffsetEXT':
		type = 'VkDescriptorSetLayout'
		z.do('auto& data = %s_index.at(%s);' % (type, toindex(type)))
		z.do('if (reader.run) data.offsets[binding] = *pOffset;')

def func_common(name, node, read, target, header, guard_header=True):
	proto = node.find('proto')
	retval = proto.find('type').text
	if name in spec.protected_funcs:
		print('#ifdef %s // %s' % (spec.protected_funcs[name], name), file=target)
		if header and guard_header:
			print('#ifdef %s // %s' % (spec.protected_funcs[name], name), file=header)
		print(file=target)
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
		if name not in vk.hardcoded and add_dummy and name not in vk.functions_noop and name not in spec.disabled_functions:
			print(file=target)
			print('#else // %s' % spec.protected_funcs[name], file=target)
			print(file=target)
			print('void retrace_%s(lava_file_reader& reader)' % name, file=target)
			print('{', file=target)
			print('\tABORT("Attempt to call unimplemented protected function: %s");' % name, file=target)
			print('}', file=target)

		print(file=target)
		print('#endif // %s 1' % spec.protected_funcs[name], file=target)
		print(file=target)
		if not add_dummy and header:
			if header: print('#endif // %s 2' % spec.protected_funcs[name], file=header)

def loadfunc(name, node, target, header):
	z = getspool()
	z.target(target)

	retval, params, paramlist = func_common(name, node, read=True, target=target, header=header, guard_header=False)
	if header:
		if name in spec.protected_funcs:
			print('#ifdef %s' % spec.protected_funcs[name], file=header)
		print('typedef void(*replay_%s_callback)(%s);' % (name, ', '.join(paramlist)), file=header)
		if name in spec.protected_funcs:
			print('#endif', file=header)
	if name in spec.disabled or name in vk.functions_noop or name in spec.disabled_functions or spec.str_contains_vendor(name):
		func_common_end(name, target=target, header=header, add_dummy=True)
		return
	if header:
		print('void retrace_%s(lava_file_reader& reader);' % name, file=header)
	if name in vk.hardcoded or name in vk.hardcoded_read:
		func_common_end(name, target=target, header=header, add_dummy=True)
		return
	print('void retrace_%s(lava_file_reader& reader)' % name, file=target)
	print('{', file=target)
	z.do('// -- Load --')
	for param in params:
		if param.inparam:
			param.print_load(param.name, '')
	if name in spec.special_count_funcs: # functions that work differently based on whether last param is a nullptr or not
		z.do('uint8_t do_call = reader.read_uint8_t();')
	z.do('// -- Execute --')
	if name in vk.extra_sync:
		z.do('sync_mutex.lock();')
	for param in params:
		if not param.inparam and param.ptr and param.type != 'void' and not name in vk.ignore_on_read and not name in spec.special_count_funcs and not name in spec.functions_create:
			vname = z.backing(param.type, param.name, size=param.length, struct=param.structure)
			z.do('%s = %s;' % (param.name, vname))
	load_add_pre(name)
	call_list = [ x.retrace_exec_param(name) for x in params ]
	if name in vk.replay_pre_calls:
		z.do('if (reader.run) replay_pre_%s(reader, %s);' % (name, ', '.join(call_list)))

	if retval == 'VkBool32': z.do('%s retval = VK_FALSE;' % retval)
	elif retval == 'VkResult': z.do('%s retval = VK_RESULT_MAX_ENUM;' % retval)
	elif retval != 'void': z.do('%s retval = (%s)0x7FFFFFFF; // hopefully an invalid value' % (retval, retval))

	if name in spec.special_count_funcs:
		if name in vk.noscreen_calls:
			z.do('if (do_call == 1 && !is_noscreen() && reader.run)')
		else:
			z.do('if (do_call == 1 && reader.run)')
		z.brace_begin()
		for vv in spec.special_count_funcs[name][2]:
			z.decl('std::vector<%s>' % vv[1], vv[0])
			call_list = call_list[:-1]
		for vv in spec.special_count_funcs[name][2]:
			call_list.append('nullptr')
		z.decl(spec.special_count_funcs[name][1] + '*', spec.special_count_funcs[name][0])
		z.do('%s = reader.pool.allocate<%s>(1);' % (spec.special_count_funcs[name][0], spec.special_count_funcs[name][1]))
		z.do('%swrap_%s(%s);' % ('retval = ' if retval == 'VkResult' else '', name, ', '.join(call_list)))
		if retval == 'VkResult': z.do('assert(retval == VK_SUCCESS);');
		for vv in spec.special_count_funcs[name][2]:
			z.do('%s.resize(*%s);' % (vv[0], spec.special_count_funcs[name][0]))
			if vv[1] in spec.type2sType:
				z.do('for (auto& i : %s) { i.sType = %s; i.pNext = nullptr; }' % (vv[0], spec.type2sType[vv[1]]))
		for vv in spec.special_count_funcs[name][2]:
			call_list = call_list[:-1]
		for vv in spec.special_count_funcs[name][2]:
			call_list.append(vv[0] + '.data()')
		z.do('%swrap_%s(%s);' % ('retval = ' if retval == 'VkResult' else '', name, ', '.join(call_list)))
		if retval == 'VkResult':
			z.do('assert(retval == VK_SUCCESS);');
			z.do('(void)retval; // ignore return value');
		z.brace_end()
		if retval == 'VkResult':
			z.do('(void)reader.read_uint32_t(); // ignore stored return value')
		assert retval in ['VkResult', 'void'], 'Unhandled retval value'
	elif name == "vkCreateInstance":
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('if (reader.run)')
		z.brace_begin()
		z.do('retval = vkuSetupInstance(%s);' % (', '.join(call_list)))
		z.do('check_retval(stored_retval, retval);')
		z.brace_end()
	elif name == "vkCreateDevice":
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('if (reader.run)')
		z.brace_begin()
		z.do('retval = vkuSetupDevice(%s);' % (', '.join(call_list)))
		z.do('check_retval(stored_retval, retval);')
		z.brace_end()
	elif name == "vkGetFenceStatus": # wait for success to restore original synchronization when call was originally successful
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_SUCCESS && reader.run) { retval = wrap_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX); }')
		z.do('else if (reader.run) { retval = wrap_vkGetFenceStatus(device, fence); }')
	elif name == "vkWaitForFences": # as above
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_SUCCESS) { timeout = UINT64_MAX; }')
		z.do('else if (stored_retval == VK_TIMEOUT) { timeout = 0; }')
		z.do('if (reader.run) retval = wrap_vkWaitForFences(device, fenceCount, pFences, waitAll, timeout);')
	elif name == "vkWaitSemaphores": # as above
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_SUCCESS;')
		z.do('if (stored_retval == VK_SUCCESS) { timeout = UINT64_MAX; }')
		z.do('else if (stored_retval == VK_TIMEOUT) { timeout = 0; }')
		z.do('if (reader.run) retval = wrap_vkWaitSemaphores(device, pWaitInfo, timeout);')
	elif name == "vkGetEventStatus": # loop until same result achieved
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_SUCCESS;')
		z.do('if (reader.run && (stored_retval == VK_EVENT_SET || stored_retval == VK_EVENT_RESET)) do { retval = wrap_vkGetEventStatus(device, event); } while (retval != stored_retval && retval != VK_ERROR_DEVICE_LOST);')
	elif name == 'vkAcquireNextImageKHR':
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_INCOMPLETE; // signal we skipped it')
		z.do('if (!is_noscreen() && (stored_retval == VK_SUCCESS || stored_retval == VK_SUBOPTIMAL_KHR) && reader.run)')
		z.brace_begin()
		z.do('retval = wrap_vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, fence, pImageIndex); // overwriting the timeout')
		z.do('auto& %s = VkSwapchainKHR_index.at(swapchainkhr_index);' % totrackable('VkSwapchainKHR'))
		z.do('%s.next_swapchain_image = *pImageIndex; // do this before we overwrite this value with stored value from file below' % totrackable('VkSwapchainKHR'))
		z.brace_end()
		z.do('else if (reader.run) cleanup_sync(index_to_VkQueue.at(0), 0, nullptr, 1, &semaphore, fence);') # just picking any queue here
	elif name == 'vkAcquireNextImage2KHR':
		z.do('const uint32_t %s = index_to_VkSwapchainKHR.index(pAcquireInfo->swapchain);' % toindex('VkSwapchainKHR'))
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_INCOMPLETE; // signal we skipped it')
		z.do('if (!is_noscreen() && reader.run && (stored_retval == VK_SUCCESS || stored_retval == VK_SUBOPTIMAL_KHR))')
		z.brace_begin()
		z.do('pAcquireInfo->timeout = UINT64_MAX; // sucess in tracing needs success in replay')
		z.do('retval = wrap_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);')
		z.do('auto& %s = VkSwapchainKHR_index.at(%s);' % (totrackable('VkSwapchainKHR'), toindex('VkSwapchainKHR')))
		z.do('%s.next_swapchain_image = *pImageIndex;' % totrackable('VkSwapchainKHR')) # do this before we overwrite this value with stored value from file
		z.brace_end()
		z.do('else if (reader.run) cleanup_sync(index_to_VkQueue.at(0), 0, nullptr, 1, &pAcquireInfo->semaphore, pAcquireInfo->fence);') # just picking any queue here
	elif name == 'vkQueuePresentKHR':
		z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
		z.do('retval = VK_SUCCESS;')
		z.do('if (!is_noscreen() && reader.run)')
		z.brace_begin()
		z.do('retval = wrap_%s(%s);' % (name, ', '.join(call_list)))
		z.brace_end()
		z.do('else if (reader.run) cleanup_sync(queue, pPresentInfo->waitSemaphoreCount, pPresentInfo->pWaitSemaphores, 0, nullptr, VK_NULL_HANDLE);')
	else:
		prefix = 'if (reader.run) '
		if name == 'vkGetQueryPoolResults':
			prefix = 'if (reader.run && !is_blackhole_mode())'
		if name in vk.layer_implemented:
			if name == 'vkSetDebugUtilsObjectNameEXT':
				prefix = 'if (wrap_%s && reader.run && pNameInfo->objectType != VK_OBJECT_TYPE_PHYSICAL_DEVICE && pNameInfo->objectType != VK_OBJECT_TYPE_DEVICE_MEMORY) ' % name
			elif name == 'vkDebugMarkerSetObjectNameEXT':
				prefix = 'if (wrap_%s && reader.run && pNameInfo->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_PHYSICAL_DEVICE_EXT && pNameInfo->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT) ' % name
			else:
				prefix = 'if (wrap_%s && reader.run) ' % name
		elif name in vk.noscreen_calls:
			prefix = 'if (!is_noscreen() && reader.run) '
		elif name in spec.functions_create and spec.functions_create[name][1] != '1':
			prefix = 'if (reader.run && stored_retval == VK_SUCCESS) '
		if retval == 'void' and not name in vk.ignore_on_read:
			z.do('%swrap_%s(%s);' % (prefix, name, ', '.join(call_list)))
		elif not name in vk.ignore_on_read:
			# stored
			if retval == 'VkResult':
				z.do('VkResult stored_retval = static_cast<VkResult>(reader.read_uint32_t());')
			elif retval in ['uint64_t', 'uint32_t']:
				z.do('%s stored_retval = reader.read_%s();' % (retval, retval))
			elif retval == 'VkBool32':
				z.do('VkBool32 stored_retval = static_cast<VkBool32>(reader.read_uint32_t());')
			elif retval in ['VkDeviceAddress', 'VkDeviceSize']:
				z.do('%s stored_retval = reader.read_uint64_t();' % retval)
			# if query succeeded in trace, make it succeed in replay (TBD: we need better fix here;
			# as this breaks dumping! yeah, it breaks if the app does this, too...)
			if name == 'vkGetQueryPoolResults':
				z.do('std::vector<char> data(dataSize);')
				call_list[5] = 'data.data()'
				z.do('if (stored_retval == VK_SUCCESS) { flags |= VK_QUERY_RESULT_WAIT_BIT; flags &= ~VK_QUERY_RESULT_PARTIAL_BIT; }')

			# current
			z.do(prefix.strip())
			z.brace_begin()
			z.do('retval = wrap_%s(%s);' % (name, ', '.join(call_list)))

			# comparison
			if retval == 'VkBool32':
				z.do('assert(stored_retval == retval);')
			elif retval == 'VkResult' and name != 'vkQueuePresentKHR':
				z.do('check_retval(stored_retval, retval);')
			elif retval in ['uint64_t', 'uint32_t']:
				z.do('assert(stored_retval == retval);')
			elif retval in ['VkDeviceAddress', 'VkDeviceSize']:
				pass
			else: assert name == 'vkQueuePresentKHR', 'Unhandled return value type %s from %s' % (retval, name)
			z.brace_end()

			# Fallback
			if retval != 'void': z.do('else retval = stored_retval;')
		else:
			if retval in ['VkResult', 'VkBool32']:
				z.do('// this function is ignored on replay')
				z.do('(void)reader.read_uint32_t(); // also ignore result return value')
	if name in vk.extra_sync:
		z.do('sync_mutex.unlock();')
	z.do('// -- Post --')
	if not name in spec.special_count_funcs and not name in vk.skip_post_calls:
		for param in params:
			if not param.inparam:
				param.print_load(param.name, '')
	if name in ['vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR']:
		z.do('auto& %s = VkSwapchainKHR_index.at(%s);' % (totrackable('VkSwapchainKHR'), toindex('VkSwapchainKHR')))
		z.do('%s.next_stored_image = *pImageIndex;' % totrackable('VkSwapchainKHR'))
	load_add_tracking(name)
	if name in vk.replay_post_calls: # hard-coded post handling
		z.do('if (reader.run) replay_post_%s(reader, %s%s);' % (name, 'retval, ' if retval != 'void' else '', ', '.join(call_list)))
	# Flexible post-handling
	if not name in spec.special_count_funcs and not name in vk.skip_post_calls and name != 'vkGetPhysicalDeviceWaylandPresentationSupportKHR':
		z.do('for (auto* c : %s_callbacks) c(%s);' % (name, ', '.join(call_list)))
	if name in vk.replay_postprocess_calls:
		z.do('if (!reader.run) replay_postprocess_%s(reader, %s%s);' % (name, 'retval, ' if retval != 'void' else '', ', '.join(call_list)))
	if name in spec.draw_commands:
		z.do('if (!reader.run) replay_postprocess_draw_command(reader, commandbuffer_index, commandbuffer_data);')
	if name in spec.compute_commands:
		z.do('if (!reader.run) replay_postprocess_compute_command(reader, commandbuffer_index, commandbuffer_data);')
	if name in spec.raytracing_commands:
		z.do('if (!reader.run) replay_postprocess_raytracing_command(reader, commandbuffer_index, commandbuffer_data);')
	z.dump()
	print('}', file=target)
	func_common_end(name, target=target, header=header, add_dummy=True)
	print(file=target)

def savefunc(name, node, target, header):
	z = getspool()
	z.target(target)

	retval, params, paramlist = func_common(name, node, read=False, target=target, header=header)
	if name in spec.disabled or name in spec.disabled_functions or spec.str_contains_vendor(name):
		func_common_end(name, target=target, header=header)
		return
	print('VKAPI_ATTR %s VKAPI_CALL trace_%s(%s);' % (retval, name, ', '.join(paramlist)), file=header)
	if name in vk.hardcoded or name in vk.hardcoded_write:
		func_common_end(name, target=target, header=header)
		return

	for i in range(len(paramlist)):
		if name in vk.deconstify and vk.deconstify[name] in paramlist[i]:
			paramlist[i] = paramlist[i] + '_ORIGINAL'
			z.first('%s %s_impl = *%s_ORIGINAL; // struct copy, discarding the const' % (params[i].type, params[i].name, params[i].name))
			z.first('%s %s = &%s_impl;' % (params[i].type + '*', params[i].name, params[i].name))
	print('VKAPI_ATTR %s VKAPI_CALL trace_%s(%s)' % (retval, name, ', '.join(paramlist)), file=target)
	print('{', file=target)
	if name in vk.functions_noop:
		print('\tassert(false);', file=target)
		z.dump()
		if retval != 'void':
			print('\treturn (%s)0;' % retval, file=target)
		print('}\n', file=target)
		func_common_end(name, target=target, header=header)
		return
	call_list = [ x.trace_exec_param(name) for x in params ]
	z.declarations.insert(0, 'lava_file_writer& writer = write_header(\"%s\", %s%s);' % (name, name.upper(), ', true' if name in spec.functions_destroy or name in vk.thread_barrier_funcs else ''))
	if name in vk.trace_pre_calls: # this must be called before we start serializing our packet, as ...
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
	if name in vk.extra_sync:
		z.do('frame_mutex.lock();')
	save_add_pre(name)
	if name == "vkCreateInstance":
		assert retval == 'VkResult'
		z.do('%s retval = VK_SUCCESS;' % retval)
		z.do('#ifdef COMPILE_LAYER')
		z.do('if (writer.run) retval = vkuSetupInstanceLayer(%s);' % (', '.join(call_list)))
		z.do('#else');
		z.do('if (writer.run) retval = vkuSetupInstance(%s);' % (', '.join(call_list)))
		z.do('#endif')
		z.do('else retval = writer.use_result.result;')
	elif name == "vkCreateDevice":
		assert retval == 'VkResult'
		z.do('%s retval = VK_SUCCESS;' % retval)
		z.do('#ifdef COMPILE_LAYER')
		z.do('if (writer.run) retval = vkuSetupDeviceLayer(%s);' % (', '.join(call_list)))
		z.do('#else');
		z.do('if (writer.run) retval = vkuSetupDevice(%s);' % (', '.join(call_list)))
		z.do('#endif')
		z.do('else retval = writer.use_result.result;')
	elif name == 'vkFlushMappedMemoryRanges':
		z.do('VkResult retval = VK_SUCCESS;')
		z.do('VkFlushRangesFlagsARM* frf = (VkFlushRangesFlagsARM*)find_extension(pMemoryRanges, VK_STRUCTURE_TYPE_FLUSH_RANGES_FLAGS_ARM);')
		z.do('if (writer.run && (!frf || !(frf->flags & VK_FLUSH_OPERATION_INFORMATIVE_BIT_ARM))) retval = wrap_%s(%s);' % (name, ', '.join(call_list)))
	elif name in vk.ignore_on_trace:
		z.do('// native call skipped')
		if retval == 'VkResult':
			z.do('VkResult retval = VK_SUCCESS;')
		assert retval in ['void', 'VkResult'], 'Bad return type for ignore case'
	else:
		extra = ('&& wrap_%s' % name) if name in vk.layer_implemented else ''
		if retval == 'VkBool32': z.do('%s retval = VK_FALSE;' % retval)
		elif retval == 'VkResult': z.do('%s retval = VK_RESULT_MAX_ENUM;' % retval)
		elif retval != 'void': z.do('%s retval = (%s)0x7FFFFFFF; // hopefully an invalid value' % (retval, retval))
		if retval != 'void':
			z.do('if (writer.run%s) retval = wrap_%s(%s);' % (extra, name, ', '.join(call_list)))
			if retval == 'VkResult': z.do('else retval = writer.use_result.result;')
			elif retval == 'VkDeviceAddress': z.do('else retval = writer.use_result.device_address;')
			elif retval == 'VkDeviceSize': z.do('else retval = writer.use_result.device_size;')
			elif retval == 'uint32_t': z.do('else retval = writer.use_result.uint_32;')
			elif retval == 'uint64_t': z.do('else retval = writer.use_result.uint_64;')
			elif retval == 'VkBool32': z.do('else retval = writer.use_result.uint_32;')
			elif retval == 'PFN_vkVoidFunction': z.do('else retval = writer.use_result.function;')
			else: assert False, 'Unhandled return type %s' % retval
		else:
			z.do('if (writer.run%s) wrap_%s(%s);' % (extra, name, ', '.join(call_list)))
	if name in vk.extra_sync:
		z.do('frame_mutex.unlock();')
	z.do('// -- Post --')
	save_add_tracking(name)
	if retval == 'VkBool32' or retval == 'VkResult' or retval == 'uint32_t':
		z.do('writer.write_uint32_t(retval);')
	elif retval in ['VkDeviceAddress', 'uint64_t', 'VkDeviceSize']:
		z.do('writer.write_uint64_t(retval);')
	if not name in spec.special_count_funcs and not name in vk.skip_post_calls:
		for param in params:
			if not param.inparam:
				param.print_save(param.name, '')

	# Push thread barriers to other threads for some functions
	if name in vk.push_thread_barrier_funcs:
		if retval == 'VkResult':
			z.do('if (retval == VK_SUCCESS || retval == VK_SUBOPTIMAL_KHR) writer.push_thread_barriers();')
		else:
			z.do('writer.push_thread_barriers();')
	if name in vk.trace_post_calls: # hard-coded post handling, must be last
		if retval != 'void':
			z.do('if (writer.run) trace_post_%s(writer, retval, %s);' % (name, ', '.join(call_list)))
		else:
			z.do('if (writer.run) trace_post_%s(writer, %s);' % (name, ', '.join(call_list)))

	if name in spec.feature_detection_funcs:
		z.do('check_%s(%s);' % (name, ', '.join(call_list)))
	elif name == 'vkBeginCommandBuffer': # special case for above, need to add the level param
		z.do('special_vkBeginCommandBuffer(commandBuffer, pBeginInfo, commandbuffer_data->level);')

	z.do('writer.thaw();')

	if retval != 'void':
		z.do('// -- Return --')
		z.do('return retval;')
	z.dump()
	print('}', file=target)
	func_common_end(name, target=target, header=header)
	print(file=target)

def convertfunc(name, node, target, header):
	z = getspool()
	z.target(target)

	retval, params, paramlist = func_common(name, node, read=False, target=target, header=header)
	if name in spec.disabled or name in spec.disabled_functions or spec.str_contains_vendor(name):
		func_common_end(name, target=target, header=header)
		return
	gfxr_paramlist = [ x.gfxr_parameter() for x in params ]

	gfxr_retval = ''

	if retval == 'void':
		print('\tvoid Process_%s(const ApiCallInfo& call_info, %s);' % (name, ', '.join(gfxr_paramlist)), file=header)
	else:
		print('\tvoid Process_%s(const ApiCallInfo& call_info, %s returnValue, %s);' % (name, retval, ', '.join(gfxr_paramlist)), file=header)
	if name in vk.hardcoded or name in vk.hardcoded_write:
		func_common_end(name, target=target, header=header)
		return
	if retval == 'void':
		print('void LavatubeConsumer::Process_%s(const ApiCallInfo& call_info, %s)' % (name, ', '.join(gfxr_paramlist)), file=target)
	else:
		print('void LavatubeConsumer::Process_%s(const ApiCallInfo& call_info, %s returnValue, %s)' % (name, retval, ', '.join(gfxr_paramlist)), file=target)
	print('{', file=target)
	call_list = [ x.trace_exec_param(name) for x in params ]
	z.declarations.insert(0, 'lava_file_writer& writer = start(\"%s\", %s%s);' % (name, name.upper(), ', true' if name in spec.functions_destroy or name in vk.thread_barrier_funcs else ''))

	for x in params:
		if not x.ptr and (x.disphandle or x.nondisphandle):
			z.first('%s %s = (%s)gfxr_%s;' % (x.type, x.name, x.type, x.name))
		if x.disphandle or x.nondisphandle: # pointer to handle
			pass # can ignore
		if x.ptr:
			z.first('%s* %s = gfxr_%s->GetPointer();' % (x.type, x.name, x.name))

	for param in params:
		if param.inparam:
			param.print_save(param.name, '', postprocess=True)
	if name in spec.special_count_funcs: # functions that work differently based on whether last param is a nullptr or not
		parlist = []
		for vv in spec.special_count_funcs[name][2]:
			parlist.append(vv[0])
		z.do('writer.write_uint8_t((%s) ? 1 : 0);' % ' && '.join(parlist))
	if retval == 'VkBool32' or retval == 'VkResult' or retval == 'uint32_t':
		z.do('writer.write_uint32_t(returnValue);')
	elif retval in ['VkDeviceAddress', 'uint64_t', 'VkDeviceSize']:
		z.do('writer.write_uint64_t(returnValue);')
	if not name in spec.special_count_funcs and not name in vk.skip_post_calls:
		for param in params:
			if not param.inparam:
				param.print_save(param.name, '', postprocess=True)

	# Push thread barriers to other threads for some functions
	if name in vk.push_thread_barrier_funcs:
		if retval == 'VkResult':
			z.do('if (returnValue == VK_SUCCESS || returnValue == VK_SUBOPTIMAL_KHR) writer.push_thread_barriers();')
		else:
			z.do('writer.push_thread_barriers();')

	if name in spec.feature_detection_funcs:
		z.do('check_%s(%s);' % (name, ', '.join(call_list)))
	elif name == 'vkBeginCommandBuffer': # special case for above, need to add the level param
		z.do('special_vkBeginCommandBuffer(commandBuffer, pBeginInfo, commandbuffer_data->level);')

	z.do('finish(writer);')
	z.dump()
	print('}', file=target)
	func_common_end(name, target=target, header=header)
	print(file=target)
