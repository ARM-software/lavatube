#!/usr/bin/python3

"""Compare tracetooltests Vulkan tests with Lavatube layer tests."""

import argparse
import pathlib
import re
import shlex
import sys


VULKAN_FUNCTIONS = {"vulkan_test", "vulkan_window_test", "vulkan_tensor_test"}


def cmake_calls(path, functions):
	text = path.read_text(encoding="utf-8")
	function_pattern = "|".join(re.escape(function) for function in functions)
	pattern = re.compile(r"^[ \t]*(" + function_pattern + r")\s*\(([^()]*)\)", re.MULTILINE)
	calls = []
	for match in pattern.finditer(text):
		arguments = match.group(2).split("#", 1)[0]
		try:
			words = shlex.split(arguments, comments=False, posix=True)
		except ValueError as error:
			raise ValueError(f"{path}:{text.count(chr(10), 0, match.start()) + 1}: {error}") from error
		if words:
			calls.append((match.group(1), words))
	return calls


def vulkan_tests(path):
	tests = []
	functions = VULKAN_FUNCTIONS | {"vulkan_test_extra"}
	for function, arguments in cmake_calls(path, functions):
		if function == "vulkan_test_extra":
			if len(arguments) < 2:
				raise ValueError(f"{path}: vulkan_test_extra() needs a test name and executable")
			tests.append(arguments[1:])
		else:
			tests.append(arguments)
	return tests


def layer_tests(path):
	tests = []
	for _function, arguments in cmake_calls(path, {"layer_test"}):
		if len(arguments) < 2:
			raise ValueError(f"{path}: layer_test() needs a test name and executable")
		tests.append(arguments[1:])
	return tests


def commented_layer_tests(path):
	text = path.read_text(encoding="utf-8")
	pattern = re.compile(r"^[ \t]*#[ \t]*layer_test\s*\(([^()]*)\)", re.MULTILINE)
	tests = []
	for match in pattern.finditer(text):
		arguments = shlex.split(match.group(1).split("#", 1)[0], comments=False, posix=True)
		if len(arguments) == 1:
			tests.append(arguments)
		elif len(arguments) >= 2:
			tests.append(arguments[1:])
	return tests


def comparable_lines(tests, executables_only):
	if executables_only:
		return sorted({test[0] for test in tests})
	return sorted({" ".join(test) for test in tests})


def parse_arguments():
	repository = pathlib.Path(__file__).resolve().parent.parent
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument(
		"--lavatube-cmake",
		type=pathlib.Path,
		default=repository / "CMakeLists.txt",
		help="top-level Lavatube CMakeLists.txt",
	)
	parser.add_argument(
		"--tracetooltests-cmake",
		type=pathlib.Path,
		default=repository / "external/tracetooltests/CMakeLists.txt",
		help="tracetooltests CMakeLists.txt",
	)
	parser.add_argument(
		"--test-cases",
		action="store_true",
		help="also compare arguments and list individual missing test cases",
	)
	parser.add_argument(
		"--check",
		action="store_true",
		help="return a non-zero status when layer tests are missing",
	)
	return parser.parse_args()


def main():
	arguments = parse_arguments()
	try:
		external = comparable_lines(
			vulkan_tests(arguments.tracetooltests_cmake), not arguments.test_cases
		)
		layer = comparable_lines(layer_tests(arguments.lavatube_cmake), not arguments.test_cases)
		commented_layer = comparable_lines(
			commented_layer_tests(arguments.lavatube_cmake), not arguments.test_cases
		)
	except (OSError, ValueError) as error:
		print(f"error: {error}", file=sys.stderr)
		return 2

	missing_set = set(external) - set(layer)
	commented = sorted(missing_set & set(commented_layer))
	missing = sorted(missing_set - set(commented))
	if not missing and not commented:
		print("All Vulkan tests have corresponding Lavatube layer tests.")
	elif not missing:
		print("No unaccounted-for Vulkan layer tests are missing.")
	elif not arguments.test_cases:
		print("Vulkan executables without a Lavatube layer test:")
		for executable in missing:
			print(f"  {executable}")
	else:
		print("Vulkan test cases without a matching Lavatube layer test:")
		grouped = {}
		for test in missing:
			executable, _separator, test_arguments = test.partition(" ")
			grouped.setdefault(executable, []).append(test_arguments)
		for executable, test_arguments_list in grouped.items():
			print(f"\n{executable}")
			for test_arguments in test_arguments_list:
				if test_arguments:
					print(f"  arguments: {test_arguments}")
				else:
					print("  arguments: (none)")

	if commented:
		print("\nCommented out tests:")
		for test in commented:
			print(f"  {test}")

	print(f"\nMissing layer tests: {len(missing)}; commented out tests: {len(commented)}")
	return 1 if arguments.check and missing else 0


if __name__ == "__main__":
	sys.exit(main())
