#!/usr/bin/python2

import spec
import util
import sys

if len(sys.argv) <= 1:
	print 'Usage: %s <function>' % sys.argv[0]
	sys.exit(-1)

for v in spec.root.findall("commands/command"):
	if v.attrib.get('alias'): continue
	proto = v.find('proto')
	name = proto.find('name').text
	if name != sys.argv[1]: continue
	util.loadfunc(name, v, sys.stdout, sys.stdout)
