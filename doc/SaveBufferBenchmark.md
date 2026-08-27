# Save-buffer performance benchmark

`tests/lava_cli_save_buffer_perf.py` measures repeated `lava-cli save buffer`
operations and writes one CSV row per measured transfer. It generates a compact trace
containing one GPU-filled buffer, starts `lava-replay --service`, runs one warm-up by
default, and reports the median, minimum, and maximum total throughput on stderr.
Performance values are observations rather than CTest pass criteria.

Build the benchmark and its dependencies with:

```sh
cmake --build build --target tracing_cli_save_buffer_perf lava-replay lava-cli -j6
```

The default invocation measures seven transfers of a 4 MiB-plus-257-byte buffer:

```sh
python3 tests/lava_cli_save_buffer_perf.py --csv save-buffer.csv
```

Use `/dev/shm` to reduce physical-storage effects on the controller side:

```sh
python3 tests/lava_cli_save_buffer_perf.py \
  --output-dir /dev/shm --destination-label tmpfs --csv save-buffer-tmpfs.csv
```

A broader size and memory-path sweep can be run with:

```sh
python3 tests/lava_cli_save_buffer_perf.py \
  --sizes 64K,1M,4194303,4194305,64M,256M \
  --memory cached,uncached,device \
  --iterations 7 --csv save-buffer-sizes.csv
```

The requested memory class does not guarantee a transfer path on every replay device.
For example, device-local memory can also be host-visible on unified-memory systems.
Always group results using the reported `path` column.

Two optional A/B dimensions are available:

```sh
python3 tests/lava_cli_save_buffer_perf.py \
  --sizes 64M --memory device \
  --staging-chunks 1M,4M,16M,64M \
  --receive-modes splice,fallback \
  --output-dir /dev/shm --destination-label tmpfs \
  --csv save-buffer-ab.csv
```

For comparisons intended to guide tuning, run shuffled rounds so system load and
thermal drift do not affect every configuration in the same fixed order. The runner
uses randomized balanced rotations of the staging-chunk and receive-mode orders. With
30 rounds, every chunk occupies each position ten times and each receive mode runs
first fifteen times per chunk. The seed makes the execution order reproducible, and
the CSV `sequence` column records the measured order:

```sh
python3 tests/lava_cli_save_buffer_perf.py \
  --sizes 64M --memory device \
  --staging-chunks 1M,4M,16M \
  --receive-modes splice,fallback \
  --warmups 1 --iterations 30 --random-seed 20260827 \
  --output-dir /dev/shm --destination-label tmpfs \
  --csv save-buffer-randomized.csv
```

One replay service is kept paused for each staging-chunk value throughout the test.
Both receive modes are measured against that service in randomized order, pairing
their results while still interleaving the staging configurations across the run.
This also keeps one replayed source buffer live per staging-chunk value, so reduce the
sweep on memory-constrained benchmark hosts.

The controls used by the runner are also available directly:

* `LAVATUBE_CLI_STAGING_CHUNK_SIZE=BYTES` changes the replay staging allocation and
  copy chunk, from 1 byte through 1 GiB. It has no effect when `path=mapped`.
* `LAVATUBE_CLI_DISABLE_SPLICE=1` makes the Linux controller use the bounded
  `recv()`/`write()` fallback.

The CSV includes requested memory class, actual replay path, size, chunk count,
requested receive mode, actual controller receive path, staging chunk size, labels,
raw trial and sequence numbers, and replay/controller/total rates and durations. The
receive path is
`splice`, `fallback`, or `mixed` when part of a transfer completed through each path.
This distinguishes an unsupported splice request that transparently used the fallback.
`--device-label`, `--driver-label`, and `--transport-label` attach experiment metadata
without changing device or network selection.

To benchmark a service running elsewhere, first pause it at a point where the buffer
is live, then pass its endpoint and the buffer index. In this mode the size, memory
class, and staging chunk are labels describing the existing service and must each
contain one value:

```sh
python3 tests/lava_cli_save_buffer_perf.py \
  --external 127.0.0.1:39091 --buffer-index 0 \
  --sizes 256M --memory device --staging-chunks 4M \
  --transport-label adb --output-dir /dev/shm \
  --csv save-buffer-adb.csv
```

The runner neither advances nor stops an external service. Configure
`LAVATUBE_CLI_STAGING_CHUNK_SIZE` in that service's environment before starting it.
The output file is written on the machine running the benchmark script, making this
mode suitable for TCP loopback, LAN endpoints, or an ADB-forwarded port.

The controller does not call `fsync()`, so destination timing ends after data reaches
the filesystem or page cache and the temporary file is renamed. It does not measure
durable storage. Large matrices write the buffer once per warm-up and trial; choose
sizes and repetitions carefully on memory- or storage-constrained systems.
