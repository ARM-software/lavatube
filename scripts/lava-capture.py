#!/usr/bin/python3
#
# Inspired by gfxreconstruct's gfxrecon-capture.py utility
#

import os
import sys
import argparse
import subprocess

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
	parser.add_argument('programAndArgs', metavar='<program> [<program args>]', nargs=argparse.REMAINDER, help='Application to capture and any program arguments')
	return parser

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
	if args.layer: os.environ['VK_LAYER_PATH'] = args.layer
	else: os.environ['VK_LAYER_PATH'] = '/opt/lavatube/implicit_layer.d'
	if args.dir is not None:
		os.chdir(args.dir)
	if not args.programAndArgs:
		parser.print_help()
		sys.exit(0)

	PrintEnvVar('LAVATUBE_DESTINATION')
	PrintEnvVar('LAVATUBE_DEBUG')
	PrintEnvVar('LAVATUBE_DEBUG_FILE')
	PrintEnvVar('VK_INSTANCE_LAYERS')
	PrintEnvVar('VK_LAYER_PATH')
	PrintEnvVar('LAVATUBE_GPU')
	PrintEnvVar('LAVATUBE_DEDICATED_BUFFER')
	PrintEnvVar('LAVATUBE_DEDICATED_IMAGE')
	PrintEnvVar('LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES')

	if args.gdb:
		result = subprocess.run(['gdb', '-q', '-ex', 'run', '--args'] + args.programAndArgs)
	else:
		result = subprocess.run(args.programAndArgs)
	sys.exit(result.returncode)
