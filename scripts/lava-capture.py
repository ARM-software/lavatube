#!/usr/bin/python3
#
# Inspired by gfxreconstruct's gfxrecon-capture.py utility
#

import os
import sys
import argparse
import subprocess
import json

def args():
	parser = argparse.ArgumentParser(prog=os.path.basename(sys.argv[0]), description='Trace a Vulkan application', allow_abbrev=False)
	parser.add_argument('--working-dir', dest='dir', metavar='<dir>', help='Run out of this directory')
	parser.add_argument('-o', dest='file', metavar='<file>', help='Name of the trace file')
	parser.add_argument('--gdb', dest='gdb', action='store_true', help='Run under gdb')
	parser.add_argument('--log-level', dest='debug', help='Debug log level, higher is more verbose')
	parser.add_argument('--log-file', dest='log', metavar='<logfile>', help='Output logs to specified file')
	parser.add_argument('--layer-path', dest='layer', metavar='<path>', help='Path to the capture layer')
	parser.add_argument('--dedicated-buffer', dest='dedbuf', action='store_true', help='Request dedicated memory allocation for buffers')
	parser.add_argument('--dedicated-image', dest='dedimg', action='store_true', help='Request dedicated memory allocation for images')
	parser.add_argument('--delayfence', dest='delayfence', metavar='<times>', help='Delay successful fence waits the given number of times')
	parser.add_argument('--gpu', dest='gpu', metavar='<gpu>', help='Use the specified GPU for tracing')
	parser.add_argument('--automate', dest='automate', action='store_true', help='Try to automate the run as much as possible if app supports CBS')
	parser.add_argument('--no-multithread', dest='nomp', action='store_true', help='Turn off multi-threaded compression and disk writeout (saves memory)')
	parser.add_argument('--trust-flushing', dest='explicit', action='store_true', help='Trust app to flush modified host memory instead of tracking usage')
	parser.add_argument('programAndArgs', metavar='<program> [<program args>]', nargs=argparse.REMAINDER, help='Application to capture and any program arguments')
	return parser

# See https://github.com/ARM-software/tracetooltests/blob/main/doc/BenchmarkingStandard.md for details
def DetectCBS(args):
	program = args.programAndArgs[0]
	tail, base = os.path.split(program)
	cap_file = None
	if program.startswith('/usr/bin/'):
		cap_file = '/usr/share/benchmarking/' + base + ".bench"
	else:
		cap_file = program + '.bench'

	if not cap_file or not os.path.exists(cap_file):
		print('No capabilities file found') # TBD remove too spammy
		return # No capabilities file

	if not os.path.isfile(cap_file) or not os.access(cap_file, os.R_OK):
		print('Capabilities file %s is not a readable' % cap_file)
		return

	if 'BENCHMARKING_ENABLE_JSON' in os.environ or 'BENCHMARKING_ENABLE_PATH' in os.environ:
		print('Existing benchmarking environment found - not overwriting')
		return

	cap_fp = open(cap_file, 'r') # should always load now
	cap_data = json.load(cap_fp)
	if not cap_data:
		print('%s is not a valid JSON file' % cap_file)
		return

	enable_file = {}
	enable_file['target'] = base
	if 'capabilities' in cap_data and args.automate:
		if 'non_interactive' in cap_data['capabilities'] and cap_data['capabilities']['non_interactive'] == 'option':
			enable_file['capabilities'] = { 'non_interactive': true }
		if 'fixed_framerate' in cap_data['capabilities'] and cap_data['capabilities']['fixed_framerate'] == 'option':
			enable_file['capabilities'] = { 'fixed_framerate': 0 }
		if 'gpu_frame_deterministic' in cap_data['capabilities'] and cap_data['capabilities']['gpu_frame_deterministic'] == 'option':
			enable_file['capabilities'] = { 'gpu_frame_deterministic': true }

	os.environ['BENCHMARKING_ENABLE_JSON'] = json.dumps(enable_file, sort_keys=True, separators=(',', ':'))

def PrintEnvVar(envVar):
	if envVar in os.environ:
		print((envVar, os.environ[envVar]))

if __name__ == '__main__':
	parser = args()
	args = parser.parse_args()
	os.environ['VK_INSTANCE_LAYERS'] = 'VK_LAYER_ARM_lavatube'
	if args.gpu: os.environ['LAVATUBE_GPU'] = args.gpu
	if args.dedbuf: os.environ['LAVATUBE_DEDICATED_BUFFER'] = '1'
	if args.dedimg: os.environ['LAVATUBE_DEDICATED_IMAGE'] = '1'
	if args.delayfence: os.environ['LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES'] = args.delayfence
	if args.debug: os.environ['LAVATUBE_DEBUG'] = args.debug
	if args.file: os.environ['LAVATUBE_DESTINATION'] = os.path.abspath(args.file)
	if args.log: os.environ['LAVATUBE_DEBUG_FILE'] = args.log
	if args.explicit: os.environ['LAVATUBE_TRUST_HOST_FLUSHING'] = '1'
	if args.layer: os.environ['VK_LAYER_PATH'] = args.layer
	else: os.environ['VK_LAYER_PATH'] = '/opt/lavatube'
	if args.nomp:
		os.environ['LAVATUBE_DISABLE_MULTITHREADED_WRITEOUT'] = '1'
		os.environ['LAVATUBE_DISABLE_MULTITHREADED_COMPRESS'] = '1'
	if args.dir is not None:
		os.chdir(args.dir)
	if not args.programAndArgs:
		parser.print_help()
		sys.exit(0)

	DetectCBS(args)

	PrintEnvVar('LAVATUBE_DESTINATION')
	PrintEnvVar('LAVATUBE_DEBUG')
	PrintEnvVar('LAVATUBE_DEBUG_FILE')
	PrintEnvVar('VK_INSTANCE_LAYERS')
	PrintEnvVar('VK_LAYER_PATH')
	PrintEnvVar('LAVATUBE_GPU')
	PrintEnvVar('LAVATUBE_DEDICATED_BUFFER')
	PrintEnvVar('LAVATUBE_DEDICATED_IMAGE')
	PrintEnvVar('LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES')
	PrintEnvVar('BENCHMARKING_ENABLE_JSON')

	if args.gdb:
		result = subprocess.run(['gdb', '-q', '-ex', 'run', '--args'] + args.programAndArgs)
	else:
		result = subprocess.run(args.programAndArgs)
	if result.returncode != 0:
		print('Captured program returned an error value!')
	sys.exit(result.returncode)
