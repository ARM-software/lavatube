#!/usr/bin/python3

import sys
sys.path.append('external/tracetooltests/scripts')
import spec
import util
import os
import argparse
import struct

# New functions that we implement
fake_functions = [ 'vkAssertBufferTRACETOOLTEST', 'vkSyncBufferTRACETOOLTEST', 'vkGetDeviceTracingObjectPropertyTRACETOOLTEST',
	'vkCmdUpdateBuffer2TRACETOOLTEST', 'vkThreadBarrierTRACETOOLTEST',
	'vkUpdateBufferTRACETOOLTEST',  'vkUpdateImageTRACETOOLTEST' ]
fake_extension_structs = {
	'VkAddressRemapTRACETOOLTEST': 'VK_STRUCTURE_TYPE_ADDRESS_REMAP_TRACETOOLTEST',
	'VkUpdateMemoryInfoTRACETOOLTEST': 'VK_STRUCTURE_TYPE_UPDATE_MEMORY_INFO_TRACETOOLTEST',
}

# Structs we want to save in our trace metadata as well
extra_tracked_structs = [ 'VkPhysicalDeviceFeatures2', 'VkPhysicalDeviceVulkan11Features', 'VkPhysicalDeviceVulkan12Features', 'VkPhysicalDeviceVulkan13Features' ]

r = open('generated/read_auto.cpp', 'w')
rh = open('generated/read_auto.h', 'w')
w = open('generated/write_auto.cpp', 'w')
wh = open('generated/write_auto.h', 'w')
u = open('generated/util_auto.cpp', 'w')
uh = open('generated/util_auto.h', 'w')
wr = open('generated/write_resource_auto.cpp', 'w')
wrh = open('generated/write_resource_auto.h', 'w')

def out(lst, str=''):
	for n in lst: print(str, file=n)

# Starts to write file output from here (w=trace, r=replay, wh=trace header, rh=replay header)
targets_all = [w,r,wh,rh,u,uh,wr,wrh]
targets_headers = [wh,rh,uh,wrh]
targets_main = [r,w]
targets_write_headers = [wh]
targets_read_headers = [rh]
targets_write = [w]
targets_read = [r]
out(targets_all, '// This file contains only code auto-generated by %s' % os.path.basename(__file__))
out(targets_all)
out(targets_headers, '#pragma once')
out(targets_headers, '#include "vulkan/vulkan.h"')
out(targets_headers, '#include "vulkan/vulkan_beta.h"')
out(targets_headers, '#include <jsoncpp/json/value.h>')
out(targets_headers, '#include "util.h"')
out(targets_headers, '#include "vkjson.h"')
out(targets_read_headers, '#include "read.h"')
out(targets_read_headers, '#include "allocators.h"')
out(targets_write_headers, '#include "write.h"')
out(targets_write_headers, '#ifdef VK_USE_PLATFORM_XCB_KHR')
out(targets_write_headers, '#include <xcb/xcb.h>')
out(targets_write_headers, '#endif')
out(targets_main, '#include <assert.h>')
out(targets_main, '#include <atomic>')
out(targets_main, '#include <vector>')
out(targets_main, '#include "tostring.h"')
out(targets_read, '#include <list>')
out(targets_main + [u,wrh], '#include "lavatube.h"')
out(targets_main + [wrh], '#include "containers.h"')
out(targets_write, '#include "util_auto.h"')
out(targets_write, '#include "vk_wrapper_auto.h"')
out([w], '#include "write_auto.h"')
out(targets_read, '#include <algorithm>')
out(targets_read, '#include "vulkan/vulkan.h"')
out(targets_read, '#include "window.h"')
out(targets_read, '#include "suballocator.h"')
out([u,wrh], '#include <unordered_map>')
out([r], '#include "read_auto.h"')
out(targets_main)
out(targets_main, '#pragma GCC diagnostic ignored "-Wunused-variable"')
out(targets_main, '#pragma GCC diagnostic ignored "-Wunused-function"')
out(targets_main, '#if (__clang_major__ > 12) || (!defined(__llvm__) && defined(__GNUC__))')
out(targets_main, '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"')
out(targets_main, '#endif')
out(targets_all)

# Debug stuff
out(targets_main, 'static bool mDebug = is_debug();')
out(targets_main)

out([wrh], 'struct trace_records')
out([wrh], '{')
out([wrh], '\ttrace_records();')
out([wrh])
for name in spec.all_handles:
	out([wrh], '\ttrace_remap<%s, %s> %s_index;' % (name, util.trackable_type_map_trace.get(name, 'trackable'), name))
	if name != 'VkDeviceMemory':
		out(targets_read, 'replay_remap<%s> index_to_%s;' % (name, name))
		out(targets_read_headers, 'extern replay_remap<%s> index_to_%s;' % (name, name))
for name in spec.all_handles:
	if name != 'VkDeviceMemory':
		out(targets_read, 'std::vector<%s> %s_index;' % (util.trackable_type_map_replay.get(name, 'trackable'), name))
		out(targets_read_headers, 'extern std::vector<%s> %s_index;' % (util.trackable_type_map_replay.get(name, 'trackable'), name))

out([wrh, wr] + targets_read)
out([wrh], '\tconst std::unordered_map<std::string, uint16_t> function_table;')
out([wrh], '\tlava_trace_func trace_getcall(const char *procname) __attribute__((pure));')
out([wr], '#include "write_resource_auto.h"')
out([wr], '#include "write_auto.h"')
out([wr])
out([wr], 'trace_records::trace_records() : function_table')
out(targets_read, 'static std::unordered_map<std::string, uint16_t> function_table =')
out(targets_read + [wr], '{')
idx = 0
for f in spec.functions:
	out(targets_read + [wr], '\t{ "%s", %d },' % (f, idx))
	idx += 1
for f in fake_functions:
	out(targets_read + [wr], '\t{ "%s", %d },' % (f, idx))
	idx += 1
out(targets_read + [wrh], '};')
out([wr], '}')
out([wr], '{}')

idx = 0
out([uh], 'enum lava_function_id')
out([uh], '{')
for f in spec.functions:
	out([uh], '\t%s, // %d' % (f.upper(), idx))
	idx += 1
for f in fake_functions:
	out([uh], '\t%s, // %d' % (f.upper(), idx))
	idx += 1
out([uh], '};')
out([uh])

out([u])
out([u], 'static std::unordered_map<uint16_t, const char*> reverse_function_table =')
out([u], '{')
idx = 0
for f in spec.functions:
	out([u], '\t{ %d, "%s" },' % (idx, f))
	idx += 1
for f in fake_functions:
	out([u], '\t{ %d, "%s" },' % (idx, f))
	idx += 1
out([u], '};')
out([u], 'const char* get_function_name(uint16_t idx) { return reverse_function_table.at(idx); }')
out([uh], 'const char* get_function_name(uint16_t idx) __attribute__((pure));')
out([uh], 'const char* get_stype_name(VkStructureType idx) __attribute__((pure));')

out([u])
out([u], 'static std::unordered_map<VkStructureType, const char*> reverse_stype_table =')
out([u], '{')
for k,v in spec.sType2type.items():
	if 'VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR' in k: continue
	if 'VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR' in k: continue
	if k in spec.protected_types:
		out(targets_read, '#ifdef %s' % (spec.protected_types[v]))
	out([u], '\t{ %s, "%s" },' % (k, v))
	if k in spec.protected_types:
		out(targets_read, '#endif')
out([u], '')
out([u], '\t{ VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, "VkLayerInstanceCreateInfo" },')
out([u], '\t{ VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, "VkLayerDeviceCreateInfo" },')
out([u], '};')
out([u], 'const char* get_stype_name(VkStructureType idx) { return reverse_stype_table.at(idx); }')

out([u])
out([uh, u])
for s in spec.feature_structs:
	if s in spec.protected_types:
		out([u], '#ifdef %s' % (spec.protected_types[s]))
	out([u], 'static void print_feature_mismatch_%s(VkPhysicalDevice physical, const %s* req)' % (s, s))
	out([u], '{')
	out([u], '\t%s host = { %s, nullptr };' % (s, spec.type2sType[s]))
	out([u], '\tVkPhysicalDeviceFeatures2 feat2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &host };')
	out([u], '\twrap_vkGetPhysicalDeviceFeatures2(physical, &feat2);')
	for v in spec.root.findall('types/type'):
		sname = v.attrib.get('name')
		if sname != s: continue
		for p in v.findall('member'):
			api = p.attrib.get('api')
			if api and api == 'vulkansc': continue
			name = p.find('name').text
			if name in ['pNext', 'sType']: continue
			out([u], '\tif (req->%s == VK_TRUE && host.%s == VK_FALSE) printf("\\t%s : %s\\n");' % (name, name, sname, name))
	out([u], '}')
	if s in spec.protected_types:
		out([u], '#endif')
	out([u])
out([uh], 'void print_pnext_feature_mismatches(VkPhysicalDevice physical, const VkDeviceCreateInfo* info);')
out([u], 'void print_pnext_feature_mismatches(VkPhysicalDevice physical, const VkDeviceCreateInfo* info)')
out([u], '{')
for s in spec.feature_structs:
	if s in spec.protected_types:
		out([u], '#ifdef %s' % (spec.protected_types[s]))
	out([u], '\tconst %s* req_%s = (const %s*)find_extension(info, %s);' % (s, s, s, spec.type2sType[s]))
	out([u], '\tif (req_%s) print_feature_mismatch_%s(physical, req_%s);' % (s, s, s))
	if s in spec.protected_types:
		out([u], '#endif')
out([u], '}')
out([uh, u])

# Generate list of all feature detection structs that we can use
out([uh], 'extern std::vector<const char*> feature_detection_callbacks;')
out([uh], 'extern std::vector<const char*> feature_detection_special_funcs;') # these need hardcoded handling
out([u], 'std::vector<const char*> feature_detection_callbacks = {')
for s in spec.feature_detection_funcs:
	out([u], '\t"%s",' % s)
out([u], '};')
out([u], 'std::vector<const char*> feature_detection_special_funcs = {')
for s in spec.feature_detection_special:
	out([u], '\t"%s",' % s)
out([u], '};')
out([u])

out([wrh, wr] + targets_read)
out(targets_read, 'static void reset_all()')
out(targets_write, 'static void reset_all(trace_records* r) REQUIRES(frame_mutex)')
out(targets_main, '{')
for v in spec.root.findall('types/type'):
	if v.attrib.get('category') == 'handle':
		if v.find('name') == None: continue
		name = v.find('name').text
		if name == 'VkDeviceMemory': continue
		if spec.str_contains_vendor(name): continue
		out(targets_write, '\tr->%s_index.clear();' % name)
		out(targets_read, '\t%s_index.clear();' % name)
		out(targets_read, '\tindex_to_%s.clear();' % name)
out(targets_main, '}')

out(targets_main)
out(targets_write, 'static void write_extension(lava_file_writer& writer, VkBaseOutStructure* sptr);')
out(targets_read, 'static void read_extension(lava_file_reader& reader, VkBaseOutStructure** sptr);')

out(targets_read, '#include "struct_read_auto.h"')
out(targets_write, '#include "struct_write_auto.h"')

out(targets_read)
out(targets_read, '#include "execute_commands.cpp"')
out(targets_read, '#include "hardcode_read.cpp"')
out(targets_read)

out(targets_write)
out(targets_write, '#include "hardcode_write.cpp"')
out(targets_write)

out(targets_read, '#include "struct_read_auto.cpp"')
out(targets_write, '#include "struct_write_auto.cpp"')

out(targets_read)
out(targets_read, 'static void read_extension(lava_file_reader& reader, VkBaseOutStructure** sptr)')
out(targets_read, '{')
out(targets_read, '\tVkStructureType pNextType = (VkStructureType)reader.read_uint32_t();')
out(targets_read, '\tassert(pNextType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && pNextType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);') # should not have been saved
out(targets_read, '\tswitch ((uint32_t)pNextType)')
out(targets_read, '\t{')
out(targets_read, '\t\tdefault: ABORT("Unhandled extension (aborting): %lu\\n", (unsigned long)pNextType); break; // unknown extension, should never have been stored')
out(targets_read, '\t\tcase 0: break; // list terminator')
for v in spec.extension_structs:
	if v in struct.skiplist: continue
	if v in spec.protected_types:
		out(targets_read, '#ifdef %s' % (spec.protected_types[v]))
	out(targets_read, '\t\tcase %s:' % spec.type2sType[v])
	out(targets_read, '\t\t{')
	out(targets_read, '\t\t\t%s* tmps = reader.pool.allocate<%s>(1);' % (v, v))
	out(targets_read, '\t\t\tmemset(tmps, 0, sizeof(%s));' % v)
	out(targets_read, '\t\t\tDLOG2("Load extension: %s (%%u)", (unsigned)pNextType);' % v)
	out(targets_read, '\t\t\tread_%s(reader, tmps);' % v)
	out(targets_read, '\t\t\tDLOG3("Done with extension: %s (%%u)", (unsigned)pNextType);' % v)
	out(targets_read, '\t\t\t*sptr = (VkBaseOutStructure*)tmps;')
	out(targets_read, '\t\t\tbreak;')
	out(targets_read, '\t\t}')
	if v in spec.protected_types:
		out(targets_read, '#endif')
for k,v in fake_extension_structs.items():
	out(targets_read, '\t\tcase %s:' % v)
	out(targets_read, '\t\t{')
	out(targets_read, '\t\t\t%s* tmps = reader.pool.allocate<%s>(1);' % (k, k))
	out(targets_read, '\t\t\tmemset(tmps, 0, sizeof(%s));' % k)
	out(targets_read, '\t\t\tDLOG2("Load fake extension: %s (%%u)", (unsigned)pNextType);' % k)
	out(targets_read, '\t\t\tread_%s(reader, tmps);' % k)
	out(targets_read, '\t\t\tDLOG3("Done with fake extension: %s (%%u)", (unsigned)pNextType);' % v)
	out(targets_read, '\t\t\t*sptr = (VkBaseOutStructure*)tmps;')
	out(targets_read, '\t\t\tbreak;')
	out(targets_read, '\t\t}')
out(targets_read, '\t}')
out(targets_read, '}')
out(targets_read)

out(targets_write)
out(targets_write, 'static void write_extension(lava_file_writer& writer, VkBaseOutStructure* sptr)')
out(targets_write, '{')
out(targets_write, '\twhile (sptr && (sptr->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || sptr->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)) sptr = sptr->pNext; // ignore these')
out(targets_write, '\tif (!sptr) { writer.write_uint32_t(0); return; }')
out(targets_write, '\tswitch ((uint32_t)sptr->sType)')
out(targets_write, '\t{')
out(targets_write, '\t\tdefault: WLOG("Unhandled extension: %lu", (unsigned long)sptr->sType); writer.write_uint32_t(0); break; // unknown extension, should not be stored')
for v in spec.extension_structs:
	if v in struct.skiplist: continue
	if v in spec.protected_types:
		out(targets_write, '#ifdef %s' % (spec.protected_types[v]))
	out(targets_write, '\t\tcase %s: DLOG2("Saving extension %s (%%u)", (unsigned)sptr->sType); writer.write_uint32_t((uint32_t)sptr->sType); write_%s(writer, (%s*)sptr); break;' % (spec.type2sType[v], v, v, v))
	if v in spec.protected_types:
		out(targets_write, '#endif')
for k,v in fake_extension_structs.items():
	out(targets_write, '\t\tcase %s: DLOG2("Saving fake extension %s (%%u)", (unsigned)sptr->sType); writer.write_uint32_t((uint32_t)sptr->sType); write_%s(writer, (%s*)sptr); break;' % (v, k, k, k))
out(targets_write, '\t}')
out(targets_write, '}')
out(targets_write)

# un-alias aliases
all_funcs = {}
for v in spec.root.findall("commands/command"):
	if not v.attrib.get('alias'):
		proto = v.find('proto')
		all_funcs[proto.find('name').text] = v
for v in spec.root.findall("commands/command"):
	if v.attrib.get('alias'):
		all_funcs[v.attrib.get('name')] = all_funcs[v.attrib.get('alias')]

# Generate all functions
for v in spec.root.findall("commands/command"):
	name = None
	api = v.attrib.get('api')
	if api and api == 'vulkansc': continue
	if v.attrib.get('alias'):
		name = v.attrib.get('name')
	else:
		proto = v.find('proto')
		name = proto.find('name').text
	if not name in spec.functions: continue
	util.loadfunc(name, all_funcs[name], r, rh)
	util.savefunc(name, all_funcs[name], w, wh)
for f in fake_extension_structs:
	out(targets_read_headers, 'void read_%s(lava_file_reader& reader, %s* sptr);' % (f, f))
for f in fake_functions:
	out(targets_read_headers, 'void retrace_%s(lava_file_reader& reader);' % f)
	if f == 'vkAssertBufferTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR uint32_t VKAPI_CALL trace_vkAssertBufferTRACETOOLTEST(VkDevice device, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);')
	elif f == 'vkSyncBufferTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void VKAPI_CALL trace_vkSyncBufferTRACETOOLTEST(VkDevice device, VkBuffer buffer);')
	elif f == 'vkGetDeviceTracingObjectPropertyTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR uint64_t VKAPI_CALL trace_vkGetDeviceTracingObjectPropertyTRACETOOLTEST(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkTracingObjectPropertyTRACETOOLTEST valueType);')
	elif f == 'vkFrameEndTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void VKAPI_CALL trace_vkFrameEndTRACETOOLTEST(VkDevice device);')
	elif f == 'vkCmdUpdateBuffer2TRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void trace_vkCmdUpdateBuffer2TRACETOOLTEST(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkUpdateMemoryInfoTRACETOOLTEST* pInfo);')
	elif f == 'vkUpdateBufferTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void trace_vkUpdateBufferTRACETOOLTEST(VkDevice device, VkBuffer buffer, VkUpdateMemoryInfoTRACETOOLTEST* pInfo);')
	elif f == 'vkUpdateImageTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void trace_vkUpdateImageTRACETOOLTEST(VkDevice device, VkImage buffer, VkUpdateMemoryInfoTRACETOOLTEST* pInfo);')
	elif f == 'vkThreadBarrierTRACETOOLTEST':
		out([wh], 'VKAPI_ATTR void trace_vkThreadBarrierTRACETOOLTEST(uint32_t count, uint32_t* pValues);')
	else:
		assert False, 'Missing fake function header implementation'

out(targets_all)

out(targets_read_headers, 'void image_update(lava_file_reader& reader, uint32_t device_index, uint32_t image_index);')
out(targets_read_headers, 'void buffer_update(lava_file_reader& reader, uint32_t device_index, uint32_t buffer_index);')
out(targets_read_headers, 'void terminate_all(lava_file_reader& reader, VkDevice device);')
out(targets_read_headers, 'void reset_for_tools();')
out(targets_read_headers)

out(targets_read, 'void retrace_init(const Json::Value& v, int heap_size, bool run)')
out(targets_read, '{')
out(targets_read, '\tint images = 0;')
out(targets_read, '\tint buffers = 0;')
out(targets_read, '\tfor (const auto& p : v.getMemberNames())')
out(targets_read, '\t{')
for v in spec.root.findall('types/type'):
	if v.attrib.get('category') == 'handle':
		if v.find('name') == None or v.find('name').text == 'VkDeviceMemory': # ignore aliases
			continue
		name = v.find('name').text
		if spec.str_contains_vendor(name): continue
		out(targets_read, '\t\tif (p == "%s")' % name)
		out(targets_read, '\t\t{')
		out(targets_read, '\t\t\tindex_to_%s.resize(v[p].asInt());' % name)
		if v.find('name').text == 'VkInstance':
			out(targets_read, '\t\t\tif (v[p].asInt() == 0) ELOG("No Vulkan instances recorded. Broken trace file!");')
		if v.find('name').text == 'VkSurfaceKHR':
			out(targets_read, '\t\t\twindow_preallocate(v[p].asInt());')
		if v.find('name').text == 'VkImage':
			out(targets_read, '\t\t\timages = v[p].asInt();')
		if v.find('name').text == 'VkBuffer':
			out(targets_read, '\t\t\tbuffers = v[p].asInt();')
		out(targets_read, '\t\t}')
out(targets_read, '\t}')
out(targets_read, '\tsuballoc_init(images, buffers, heap_size, !run);')
out(targets_read, '}')
out(targets_read_headers, 'void retrace_init(const Json::Value& v, int heap_size = -1, bool run = true);')

out(targets_write)
out(targets_write, 'Json::Value trace_limits(const lava_writer* instance)')
out(targets_write, '{')
out(targets_write, '\tJson::Value v;')
for v in spec.root.findall('types/type'):
	if v.attrib.get('category') == 'handle':
		if v.find('name') == None: # ignore aliases
			continue
		name = v.find('name').text
		if spec.str_contains_vendor(name): continue
		out(targets_write, '\tv["%s"] = (unsigned)instance->records.%s_index.size();' % (name, name))
out(targets_write, '\treturn v;')
out(targets_write, '}')
out(targets_write_headers, 'Json::Value trace_limits(const lava_writer* instance) REQUIRES(frame_mutex);')

out(targets_read)
out(targets_read, 'lava_replay_func retrace_getcall(uint16_t call)')
out(targets_read, '{')
out(targets_read, '\tswitch(call)')
out(targets_read, '\t{')
idx = 0
for f in spec.functions:
	if f in util.functions_noop or f in spec.disabled_functions or spec.str_contains_vendor(f):
		out(targets_read, '\tcase %d:' % idx)
		out(targets_read, '\t\tDLOG3("Attempt to use retrace_getcall on unimplemented function %s with index %d.");' % (f, idx))
		out(targets_read, '\t\treturn nullptr;')
	else:
		out(targets_read, '\tcase %d: return (lava_replay_func)retrace_%s;' % (idx, f))
	idx += 1
for f in fake_functions:
	out(targets_read, '\tcase %d: return (lava_replay_func)retrace_%s;' % (idx, f))
	idx += 1
out(targets_read, '\tdefault: return nullptr;')
out(targets_read, '\t}')
out(targets_read, '}')
out([rh], 'lava_replay_func retrace_getcall(uint16_t call);')

out([wr])
out([wr], 'lava_trace_func trace_records::trace_getcall(const char *procname)')
out([wr], '{')
out([wr], '\tuint16_t idx;')
out([wr], '\tif (function_table.count(procname) == 0)')
out([wr], '\t{')
out([wr], '\t\tDLOG("LAVATUBE WARNING: Failed to map procname %s to function index. Returning nullptr.", procname);')
out([wr], '\t\tidx=UINT16_MAX;')
out([wr], '\t} else {')
out([wr], '\t\tidx = function_table.at(procname);')
out([wr], '\t}')
out([wr])
out([wr], '\tswitch(idx)')
out([wr], '\t{')
idx = 0
for f in spec.functions:
	if f in spec.protected_funcs:
		out([wr], '#ifdef %s' % spec.protected_funcs[f])
	if f in util.functions_noop or f in spec.disabled_functions or spec.str_contains_vendor(f):
		out([wr], '\tcase %d:' % idx)
		out([wr], '\t\tDLOG("Attempt to use trace_getcall on unimplemented function %s with index %s.");' % (f, idx))
		out([wr], '\t\treturn nullptr;')
	elif f in spec.function_aliases:
		out([wr], '\tcase %d: return (lava_trace_func)trace_%s; // alias for %s' % (idx, f, spec.function_aliases[f]))
	else:
		out([wr], '\tcase %d: return (lava_trace_func)trace_%s;' % (idx, f))
	if f in spec.protected_funcs:
		out([wr], '#endif')
	idx += 1
for f in fake_functions:
	out([wr], '\tcase %d: return (lava_trace_func)trace_%s;' % (idx, f))
	idx += 1
out([wr], '\tdefault: return nullptr;')
out([wr], '\t}')
out([wr], '}')

out(targets_read)
out(targets_read, 'uint16_t retrace_getid(const char* f)')
out(targets_read, '{')
out(targets_read, '\tif (function_table.count(f) == 0) return UINT16_MAX;')
out(targets_read, '\treturn function_table.at(f);')
out(targets_read, '}')
out(targets_read_headers, 'uint16_t retrace_getid(const char* f);')

out(targets_write)
out(targets_write_headers, 'Json::Value trackable_json(const lava_writer* instance) REQUIRES(frame_mutex);')
out(targets_write, 'Json::Value trackable_json(const lava_writer* instance)')
out(targets_write, '{')
out(targets_write, '\tJson::Value v;')

for v in spec.root.findall('types/type'):
	if v.attrib.get('category') == 'handle':
		if v.find('name') == None: # ignore aliases
			continue
		name = v.find('name').text
		if spec.str_contains_vendor(name): continue
		out(targets_write, '\tif (instance->records.%s_index.size())' % name)
		out(targets_write, '\t{')
		out(targets_write, '\t\tv["%s"] = Json::arrayValue;' % name)
		out(targets_write, '\t\tfor (const %s* data : instance->records.%s_index.iterate())' % (util.trackable_type_map_trace.get(name, 'trackable'), name))
		out(targets_write, '\t\t{')
		out(targets_write, '\t\t\tJson::Value vv = %s_json(data);' % util.trackable_type_map_trace.get(name, 'trackable'))
		out(targets_write, '\t\t\tvv["index"] = data->index;')
		out(targets_write, '\t\t\tv["%s"].append(vv);' % name)
		out(targets_write, '\t\t}')
		out(targets_write, '\t}')
for e in extra_tracked_structs:
	out(targets_write, '\tif (instance->meta.app.stored_%s) v["%s"] = write%s(*instance->meta.app.stored_%s);' % (e, e, e, e))
out(targets_write, '\treturn v;')
out(targets_write, '}')

out(targets_read)
out(targets_read_headers, 'void trackable_read(const Json::Value& v);')
out(targets_read, 'void trackable_read(const Json::Value& v)')
out(targets_read, '{')
for v in spec.root.findall('types/type'):
	if v.attrib.get('category') == 'handle':
		if v.find('name') == None or v.find('name').text == 'VkDeviceMemory': # ignore aliases
			continue
		name = v.find('name').text
		if spec.str_contains_vendor(name): continue
		out(targets_read, '\tif (v.isMember("%s")) for (const auto& i : v["%s"]) %s_index.push_back(%s_json(i));' % (name, name, name, util.trackable_type_map_replay.get(name, 'trackable')))
for e in extra_tracked_structs:
	out(targets_read, '\tif (v.isMember("%s")) { has_%s = true; read%s(v["%s"], stored_%s); }' % (e, e, e, e, e))
out(targets_read, '}')

# Make callbacks
out(targets_read)
for f in spec.functions:
	if f in spec.protected_funcs:
		out(targets_read + targets_read_headers, '#ifdef %s' % (spec.protected_funcs[f]))
	out(targets_read, 'std::vector<replay_%s_callback> %s_callbacks;' % (f, f))
	out(targets_read_headers, 'extern std::vector<replay_%s_callback> %s_callbacks;' % (f, f))
	if f in spec.protected_funcs:
		out(targets_read + targets_read_headers, '#endif')

# Clean up
for n in targets_all:
	n.close()
