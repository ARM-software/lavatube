#!/usr/bin/python2

import spec
import re
import sys

source = open('generated/tostring.cpp', 'w')
header = open('generated/tostring.h', 'w')
assert source, 'Could not create source file'
assert header, 'Could not create header file'

print >> header, '// This file contains only auto-generated code!'
print >> header
print >> header, '#pragma once'
print >> header, '#include "util.h"'
print >> header

print >> source, '// This file contains only auto-generated code!'
print >> source
print >> source, '#include "tostring.h"'
print >> source, '#include <string.h>'
print >> source

bitmask = {}
missing = []
protected = {}
platforms = {}

spec.init()

# Find all platforms
for v in spec.root.findall('platforms/platform'):
	name = v.attrib.get('name')
	prot = v.attrib.get('protect')
	platforms[name] = prot

# Find all flag conversions
for v in spec.root.findall('types/type'):
	category = v.attrib.get('category')
	requires = v.attrib.get('requires')
	api = v.attrib.get('api')
	if api and api == 'vulkansc': continue
	if category == 'bitmask':
		# ignore aliases for now
		if v.find('name') == None:
			continue
		name = v.find('name').text
		if requires:
			bitmask[requires] = name
		else:
			missing.append(name)

# Find ifdef conditionals (that we want to recreate) and disabled extensions (that we want to ignore)
for v in spec.root.findall('extensions/extension'):
	name = v.attrib.get('name')
	conditional = v.attrib.get('platform')
	if conditional:
		for n in v.findall('require/type'):
			protected[n.attrib.get('name')] = platforms[conditional]

# -- Bit to string --

added_case = []
for v in spec.root.findall('enums'):
	rname = v.attrib.get('name')
	type = v.attrib.get('type')
	if not type or type != 'bitmask':
		continue
	if not rname in bitmask or not rname in spec.types:
		continue
	ename = bitmask[rname]
	if ename in protected:
		print >> header, '#ifdef %s // %s' % (protected[ename], rname)
		print >> source, '#ifdef %s // %s' % (protected[ename], rname)
	print >> header, 'std::string %s_to_string(%s flags);' % (ename, ename)
	print >> source, 'std::string %s_to_string(%s flags)' % (ename, ename)
	print >> source, '{'
	print >> source, '\tstd::string result;'
	print >> source, '\twhile (flags)'
	print >> source, '\t{'
	print >> source, '\t\tint bit = 0;'
	print >> source, '\t\twhile (!(flags & 1 << bit)) bit++; // find first set bit'
	print >> source, '\t\tswitch (flags & (1 << bit))'
	print >> source, '\t\t{'
	for bit in v.findall('enum'):
		name = bit.attrib.get('name')
		pos = bit.attrib.get('bitpos')
		if pos and not name in added_case:
			print >> source, '\t\tcase %s: result += "%s"; break;' % (name, name)
			added_case.append(name)
	# Find and add extensions enums
	for ext in spec.root.findall('extensions/extension'):
		supported = ext.attrib.get('supported')
		if supported in ['disabled', 'vulkansc']: continue
		for vv in ext.findall('require'):
			for bit in vv.findall('enum'):
				extends = bit.attrib.get('extends')
				if extends == ename:
					name = bit.attrib.get('name')
					pos = bit.attrib.get('bitpos')
					if pos and not name in added_case:
						print >> source, '\t\tcase %s: result += "%s"; break;' % (name, name)
						added_case.append(name)
	print >> source, '\t\tdefault: result += "Bad bitfield value"; break;'
	print >> source, '\t\t}'
	print >> source, '\t\tflags &= ~(1 << bit); // remove bit'
	print >> source, '\t\tif (flags) result += " | ";'
	print >> source, '\t}'
	print >> source, '\treturn result;'
	print >> source, '}'
	if ename in protected:
		print >> header, '#endif'
		print >> source, '#endif'
	print >> source

for name in missing: # create stubs for unused flags
	if name in protected:
		print >> header, '#ifdef %s // %s' % (protected[name], rname)
	print >> header, 'static inline std::string %s_to_string(%s flags) { return std::string(); }' % (name, name)
	if name in protected:
		print >> header, '#endif'

# -- Enum to string --

print >> header

added_case = []
for v in spec.root.findall('enums'):
	name = v.attrib.get('name')
	type = v.attrib.get('type')
	if not type or type != 'enum' or not name in spec.types:
		continue
	if name in protected:
		print >> header, '#ifdef %s // %s' % (protected[name], name)
		print >> source, '#ifdef %s // %s' % (protected[name], name)
	print >> header, 'std::string %s_to_string(%s val);' % (name, name)
	print >> source, 'std::string %s_to_string(%s val)' % (name, name)
	print >> source, '{'
	print >> source, '\tswitch (val)'
	print >> source, '\t{'
	for item in v.findall('enum'):
		itemname = item.attrib.get('name')
		if item.attrib.get('alias', None):
			continue
		print >> source, '\tcase %s: return "%s";' % (itemname, itemname)
		added_case.append(itemname)
	# Find and add extensions enums
		supported = ext.attrib.get('supported')
		if supported in ['disabled', 'vulkansc']: continue
	for vv in spec.root.findall('extensions/extension'):
		extname = vv.attrib.get('name')
		if vv.attrib.get('supported') in ['disabled', 'vulkansc']:
			continue
		for item in vv.findall('require/enum'):
			extends = item.attrib.get('extends')
			itemname = item.attrib.get('name')
			if extends == name and not itemname in added_case:
				# FIXME need to handle deduplication of aliases, not trivial from spec
				#print >> source, '\tcase %s: return "%s"; // from %s' % (itemname, itemname, extname)
				added_case.append(itemname)
	print >> source, '\tdefault: return "Unhandled enum";'
	print >> source, '\t}'
	print >> source, '\treturn "Error";'
	print >> source, '}'
	if name in protected:
		print >> header, '#endif'
		print >> source, '#endif'
	print >> source
