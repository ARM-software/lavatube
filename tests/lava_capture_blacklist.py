#!/usr/bin/python3

import os
import subprocess
import sys
import unittest


class LavaCaptureBlacklistTest(unittest.TestCase):
	def setUp(self):
		self.capture = os.path.join(os.path.dirname(__file__), '..', 'scripts', 'lava-capture.py')

	def run_capture(self, arguments, inherited=None):
		environment = os.environ.copy()
		if inherited is not None:
			environment['LAVATUBE_BLACKLIST_EXTENSIONS'] = inherited
		return subprocess.run(
			[sys.executable, self.capture] + arguments + ['/usr/bin/env'],
			env=environment,
			text=True,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
		)

	def test_cli_normalizes_and_overrides_environment(self):
		result = self.run_capture(
			['--blacklist-extensions', ' VK_EXT_debug_utils, VK_ARM_trace_helpers,VK_EXT_debug_utils '],
			'VK_KHR_swapchain',
		)
		self.assertEqual(result.returncode, 0, result.stderr)
		self.assertIn('LAVATUBE_BLACKLIST_EXTENSIONS=VK_EXT_debug_utils,VK_ARM_trace_helpers', result.stdout)
		self.assertNotIn('LAVATUBE_BLACKLIST_EXTENSIONS=VK_KHR_swapchain', result.stdout)

	def test_inherited_environment_is_normalized(self):
		result = self.run_capture([], ' VK_EXT_debug_utils, VK_EXT_debug_utils ')
		self.assertEqual(result.returncode, 0, result.stderr)
		self.assertIn('LAVATUBE_BLACKLIST_EXTENSIONS=VK_EXT_debug_utils', result.stdout)

	def test_malformed_list_is_rejected_before_launch(self):
		result = self.run_capture(['--blacklist-extensions', 'VK_EXT_debug_utils,,bad'])
		self.assertNotEqual(result.returncode, 0)
		self.assertIn('invalid Vulkan extension name', result.stderr)


if __name__ == '__main__':
	unittest.main()
