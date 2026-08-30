#!/usr/bin/python3

import argparse
import csv
import os
import random
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import time


SIZE_PATTERN = re.compile(r'^(\d+)([KMGkmg]?)$')
SUMMARY_PATTERN = re.compile(r'^DONE bytes=(\d+) path=([a-z]+) chunks=(\d+)$')
RATE_PATTERN = re.compile(r'^(replay|readback|send|controller|total)=([0-9.]+) MiB/s time=([0-9.]+) s$')


def parse_size(text):
	match = SIZE_PATTERN.match(text)
	if not match:
		raise argparse.ArgumentTypeError('size must be bytes or use a K, M, or G suffix: %s' % text)
	value = int(match.group(1))
	multiplier = {'': 1, 'k': 1024, 'm': 1024 * 1024, 'g': 1024 * 1024 * 1024}[match.group(2).lower()]
	value *= multiplier
	if value < 4:
		raise argparse.ArgumentTypeError('size must be at least four bytes')
	return value


def parse_size_list(text):
	return [parse_size(value.strip()) for value in text.split(',') if value.strip()]


def parse_choice_list(text, choices, name):
	values = [value.strip() for value in text.split(',') if value.strip()]
	if not values or any(value not in choices for value in values):
		raise argparse.ArgumentTypeError('%s must be a comma-separated selection from %s' % (name, ','.join(choices)))
	return values


def parse_endpoint(text):
	try:
		host, port_text = text.rsplit(':', 1)
		port = int(port_text)
	except ValueError:
		raise argparse.ArgumentTypeError('external service must use HOST:PORT')
	if not host or port < 1 or port > 65535:
		raise argparse.ArgumentTypeError('external service must use HOST:PORT with a valid port')
	return host, port


def reserve_port():
	with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
		sock.bind(('127.0.0.1', 0))
		return sock.getsockname()[1]


def run_cli(cli, host, port, command, environment, timeout):
	return subprocess.run(
		[cli, '-H', host, '-P', str(port)] + command,
		env=environment,
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=timeout,
	)


def wait_for_listener(replay, port, log_file):
	deadline = time.monotonic() + 30
	while time.monotonic() < deadline:
		if replay.poll() is not None:
			log_file.seek(0)
			raise RuntimeError('lava-replay exited before opening its service port:\n' + log_file.read().decode(errors='replace'))
		try:
			with socket.create_connection(('127.0.0.1', port), timeout=0.1):
				return
		except OSError:
			time.sleep(0.005)
	raise RuntimeError('timed out waiting for lava-replay service')


def stop_replay(replay, cli, port, log_file, environment, timeout):
	if replay.poll() is None:
		try:
			run_cli(cli, '127.0.0.1', port, ['stop'], environment, timeout)
		except (OSError, subprocess.TimeoutExpired):
			pass
	try:
		replay.wait(timeout=30)
	except subprocess.TimeoutExpired:
		replay.terminate()
		replay.wait(timeout=5)
	if replay.returncode != 0:
		log_file.seek(0)
		raise RuntimeError('lava-replay exited with %d:\n%s' %
		                   (replay.returncode, log_file.read().decode(errors='replace')))


def parse_save_output(output, expected_size):
	lines = output.splitlines()
	if len(lines) != 6:
		raise RuntimeError('unexpected save output: %r' % output)
	summary = SUMMARY_PATTERN.match(lines[0])
	if not summary or int(summary.group(1)) != expected_size:
		raise RuntimeError('unexpected save summary: %r' % lines[0])
	values = {
		'path': summary.group(2),
		'chunks': int(summary.group(3)),
	}
	for line in lines[1:]:
		match = RATE_PATTERN.match(line)
		if not match:
			raise RuntimeError('unexpected save rate: %r' % line)
		values[match.group(1) + '_mib_s'] = float(match.group(2))
		values[match.group(1) + '_seconds'] = float(match.group(3))
	return values


def create_trace(generator, trace_base, size, memory_class, environment, timeout):
	env = dict(environment)
	env['LAVATUBE_DESTINATION'] = trace_base
	result = subprocess.run(
		[generator, str(size), memory_class],
		env=env,
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		timeout=timeout,
	)
	if result.returncode == 77:
		return False
	if result.returncode != 0:
		raise RuntimeError('trace generator failed (rc=%d):\nstdout:\n%s\nstderr:\n%s' %
		                   (result.returncode, result.stdout, result.stderr))
	trace = trace_base + '.api'
	if not os.path.exists(trace):
		raise RuntimeError('trace generator did not create %s' % trace)
	return trace

def run_trial(args, host, port, buffer_index, size, memory_class, staging_chunk,
              trial, output_file, rows, writer):
	result = run_cli(args.cli, host, port, ['save', 'buffer', str(buffer_index), output_file], os.environ, args.timeout)
	if result.returncode != 0:
		raise RuntimeError('save failed: rc=%d stdout=%r stderr=%r' %
		                   (result.returncode, result.stdout, result.stderr))
	values = parse_save_output(result.stdout, size)
	if trial is not None:
		row = {
			'device': args.device_label,
			'driver': args.driver_label,
			'size': size,
			'memory': memory_class,
			'path': values['path'],
			'chunks': values['chunks'],
			'transport': args.transport_label,
			'destination': args.destination_label,
			'staging_chunk': staging_chunk,
			'staging_buffers': args.staging_buffers,
			'trial': trial,
			'sequence': len(rows) + 1,
			'replay_mib_s': values['replay_mib_s'],
			'readback_mib_s': values['readback_mib_s'],
			'send_mib_s': values['send_mib_s'],
			'controller_mib_s': values['controller_mib_s'],
			'total_mib_s': values['total_mib_s'],
			'replay_seconds': values['replay_seconds'],
			'readback_seconds': values['readback_seconds'],
			'send_seconds': values['send_seconds'],
			'controller_seconds': values['controller_seconds'],
			'total_seconds': values['total_seconds'],
		}
		rows.append(row)
		writer.writerow(row)


def run_trials(args, host, port, buffer_index, size, memory_class, staging_chunk, output_file, rows, writer):
	for trial in range(args.warmups + args.iterations):
		measured_trial = None if trial < args.warmups else trial - args.warmups + 1
		run_trial(args, host, port, buffer_index, size, memory_class, staging_chunk,
		          measured_trial, output_file, rows, writer)


def start_benchmark_service(args, trace, staging_chunk):
	port = reserve_port()
	replay_env = dict(os.environ)
	replay_env['LAVATUBE_CLI_STAGING_CHUNK_SIZE'] = str(staging_chunk)
	replay_env['LAVATUBE_CLI_STAGING_BUFFER_COUNT'] = str(args.staging_buffers)
	log_file = tempfile.TemporaryFile()
	replay = subprocess.Popen(
		[args.replay, '--service', '-H', '127.0.0.1', '-P', str(port), '-w', 'none', trace],
		stdout=log_file,
		stderr=subprocess.STDOUT,
		env=replay_env,
	)
	try:
		wait_for_listener(replay, port, log_file)
		goto = run_cli(args.cli, '127.0.0.1', port, ['goto', '0', 'vkDeviceWaitIdle'], os.environ, args.timeout)
		if goto.returncode != 0 or 'name=vkDeviceWaitIdle ' not in goto.stdout:
			raise RuntimeError('failed to reach benchmark checkpoint: stdout=%r stderr=%r' %
			                   (goto.stdout, goto.stderr))
	except Exception:
		try:
			stop_replay(replay, args.cli, port, log_file, os.environ, args.timeout)
		finally:
			log_file.close()
		raise
	return replay, port, log_file


def stop_benchmark_service(args, service):
	replay, port, log_file = service
	try:
		stop_replay(replay, args.cli, port, log_file, os.environ, args.timeout)
	finally:
		log_file.close()


def benchmark_service(args, trace, size, memory_class, staging_chunk, output_file, rows, writer):
	service = start_benchmark_service(args, trace, staging_chunk)
	try:
		port = service[1]
		run_trials(args, '127.0.0.1', port, 0, size, memory_class, staging_chunk, output_file, rows, writer)
	finally:
		stop_benchmark_service(args, service)


def balanced_orders(values, count, randomizer):
	orders = []
	while len(orders) < count:
		base = list(values)
		randomizer.shuffle(base)
		batch = [base[offset:] + base[:offset] for offset in range(len(base))]
		randomizer.shuffle(batch)
		orders.extend(batch)
	return orders[:count]


def randomized_rounds(args, count, randomizer):
	chunk_orders = balanced_orders(args.staging_chunks, count, randomizer)
	return [chunk_orders[index] for index in range(count)]


def benchmark_randomized(args, trace, size, memory_class, output_file, rows, writer):
	randomizer = random.Random(args.random_seed)
	rounds = randomized_rounds(args, args.warmups, randomizer)
	rounds.extend(randomized_rounds(args, args.iterations, randomizer))
	services = {}
	failure = None
	try:
		for staging_chunk in args.staging_chunks:
			services[staging_chunk] = start_benchmark_service(args, trace, staging_chunk)
		for round_index, staging_chunks in enumerate(rounds):
			measured_trial = None if round_index < args.warmups else round_index - args.warmups + 1
			if measured_trial is None:
				label = 'warmup %d/%d' % (round_index + 1, args.warmups)
			else:
				label = 'trial %d/%d' % (measured_trial, args.iterations)
			print('randomized %s chunks=%s' %
			      (label, ','.join(str(value) for value in staging_chunks)), file=sys.stderr)
			for staging_chunk in staging_chunks:
				port = services[staging_chunk][1]
				run_trial(args, '127.0.0.1', port, 0, size, memory_class, staging_chunk,
				          measured_trial, output_file, rows, writer)
	except Exception as error:
		failure = error
	finally:
		for service in services.values():
			try:
				stop_benchmark_service(args, service)
			except Exception as error:
				if failure is None:
					failure = error
	if failure is not None:
		raise failure


def print_summary(rows):
	groups = {}
	for row in rows:
		key = (row['size'], row['memory'], row['path'], row['staging_chunk'], row['staging_buffers'])
		groups.setdefault(key, []).append(row)
	for key in sorted(groups):
		group = groups[key]
		total_values = [row['total_mib_s'] for row in group]
		readback_values = [row['readback_mib_s'] for row in group]
		send_values = [row['send_mib_s'] for row in group]
		print('size=%d memory=%s path=%s staging=%d buffers=%d total_MiB/s median=%.2f min=%.2f max=%.2f readback=%.2f send=%.2f' %
		      (key[0], key[1], key[2], key[3], key[4], statistics.median(total_values),
		       min(total_values), max(total_values), statistics.median(readback_values), statistics.median(send_values)),
		      file=sys.stderr)


def main():
	parser = argparse.ArgumentParser(description='Benchmark lava-cli save buffer and write raw trials as CSV.')
	parser.add_argument('--generator', default='build/tracing_cli_save_buffer_perf')
	parser.add_argument('--replay', default='build/lava-replay')
	parser.add_argument('--cli', default='build/lava-cli')
	parser.add_argument('--external', type=parse_endpoint, metavar='HOST:PORT',
	                    help='benchmark an already-paused service instead of launching lava-replay')
	parser.add_argument('--buffer-index', type=int, default=0,
	                    help='buffer index used with --external (default: 0)')
	parser.add_argument('--sizes', type=parse_size_list, default=parse_size_list('4194561'),
	                    help='comma-separated byte sizes with optional K/M/G suffix')
	parser.add_argument('--memory', default='device',
	                    help='comma-separated cached,uncached,device classes')
	parser.add_argument('--staging-chunks', type=parse_size_list, default=parse_size_list('4M'),
	                    help='comma-separated staging chunk sizes')
	parser.add_argument('--staging-buffers', type=int, choices=(1, 2), default=2,
	                    help='number of replay staging slots (default: 2)')
	parser.add_argument('--warmups', type=int, default=1)
	parser.add_argument('--iterations', type=int, default=7)
	parser.add_argument('--random-seed', type=int,
	                    help='run shuffled rounds using this reproducible seed')
	parser.add_argument('--timeout', type=int, default=300)
	parser.add_argument('--output-dir', help='parent directory for downloaded files, e.g. /dev/shm')
	parser.add_argument('--csv', default='-', help='CSV output file, or - for stdout')
	parser.add_argument('--device-label', default='')
	parser.add_argument('--driver-label', default='')
	parser.add_argument('--transport-label', default='loopback')
	parser.add_argument('--destination-label', default='temporary-file')
	args = parser.parse_args()
	try:
		args.memory = parse_choice_list(args.memory, ('cached', 'uncached', 'device'), 'memory')
	except argparse.ArgumentTypeError as error:
		parser.error(str(error))
	if not args.sizes or not args.staging_chunks:
		parser.error('sizes and staging chunks must not be empty')
	if args.warmups < 0 or args.iterations <= 0 or args.timeout <= 0:
		parser.error('warmups must be non-negative and iterations/timeout must be positive')
	if args.buffer_index < 0:
		parser.error('buffer index must be non-negative')
	if args.external and (len(args.sizes) != 1 or len(args.memory) != 1 or len(args.staging_chunks) != 1):
		parser.error('--external requires exactly one size, memory class, and staging chunk label')
	if args.external and args.random_seed is not None:
		parser.error('--random-seed cannot be used with --external')
	if args.output_dir and not os.path.isdir(args.output_dir):
		parser.error('output directory does not exist: %s' % args.output_dir)

	fieldnames = [
		'device', 'driver', 'size', 'memory', 'path', 'chunks', 'transport', 'destination',
		'staging_chunk', 'staging_buffers', 'trial', 'sequence',
		'replay_mib_s', 'readback_mib_s', 'send_mib_s', 'controller_mib_s', 'total_mib_s',
		'replay_seconds', 'readback_seconds', 'send_seconds', 'controller_seconds', 'total_seconds',
	]
	csv_file = sys.stdout if args.csv == '-' else open(args.csv, 'w', newline='')
	rows = []
	try:
		writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
		writer.writeheader()
		with tempfile.TemporaryDirectory(prefix='lava_cli_save_perf_output_', dir=args.output_dir) as output_dir:
			output_file = os.path.join(output_dir, 'buffer.bin')
			if args.external:
				run_trials(args, args.external[0], args.external[1], args.buffer_index,
				           args.sizes[0], args.memory[0], args.staging_chunks[0],
				           output_file, rows, writer)
				csv_file.flush()
			else:
				with tempfile.TemporaryDirectory(prefix='lava_cli_save_perf_traces_') as trace_dir:
					for size in args.sizes:
						for memory_class in args.memory:
							trace_base = os.path.join(trace_dir, 'buffer_%d_%s' % (size, memory_class))
							trace = create_trace(args.generator, trace_base, size, memory_class, os.environ, args.timeout)
							if not trace:
								print('SKIP size=%d memory=%s: memory class unavailable' % (size, memory_class), file=sys.stderr)
								continue
							if args.random_seed is not None:
								benchmark_randomized(args, trace, size, memory_class, output_file, rows, writer)
								csv_file.flush()
							else:
								for staging_chunk in args.staging_chunks:
									benchmark_service(args, trace, size, memory_class, staging_chunk, output_file, rows, writer)
									csv_file.flush()
	finally:
		if csv_file is not sys.stdout:
			csv_file.close()
	print_summary(rows)
	if not rows:
		return 77
	return 0


if __name__ == '__main__':
	sys.exit(main())
