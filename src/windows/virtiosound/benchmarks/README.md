# Virtio-sound core benchmark

This benchmark compares steady-state `VirtioSoundDevice::PumpCapture` work in
the baseline and candidate source trees. It uses the same harness for both
builds; `WSL_VIRTIO_SOUND_SOURCE_DIR` selects the production core to compile.
Run the commands from the candidate checkout root so both runs use the harness
at `src/windows/virtiosound/tests/VirtioSoundDevicePerfBenchmark.cpp`.

## Windows build and run

Use separate build directories so each CMake cache keeps its own source tree.
The source directory passed to `WSL_VIRTIO_SOUND_SOURCE_DIR` must be the
`src/windows/virtiosound` directory in the selected checkout. The WIL include
directory must contain `wil/resource.h`.

```powershell
$harness = (Resolve-Path "src/windows/virtiosound/benchmarks").Path
$candidateSource = (Resolve-Path "src/windows/virtiosound").Path
$baselineSource = (Resolve-Path (Read-Host "Path to baseline src/windows/virtiosound")).Path
$wilInclude = (Resolve-Path (Read-Host "Path to the WIL include directory")).Path

cmake -S $harness -B out/virtio-sound-bench-baseline `
  -G "Visual Studio 17 2022" -A x64 `
  "-DWSL_VIRTIO_SOUND_SOURCE_DIR=$baselineSource" `
  "-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=$wilInclude"
cmake --build out/virtio-sound-bench-baseline --config Release --parallel 1
& .\out\virtio-sound-bench-baseline\Release\virtio_sound_core_benchmark.exe 20000

cmake -S $harness -B out/virtio-sound-bench-candidate `
  -G "Visual Studio 17 2022" -A x64 `
  "-DWSL_VIRTIO_SOUND_SOURCE_DIR=$candidateSource" `
  "-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=$wilInclude"
cmake --build out/virtio-sound-bench-candidate --config Release --parallel 1
& .\out\virtio-sound-bench-candidate\Release\virtio_sound_core_benchmark.exe 20000
```

For another Visual Studio version or architecture, use the same generator and
architecture for both builds. If the iteration count is omitted, the harness
runs 20,000 periods; accepted values are 1 through 1,000,000. Use the same
count for both binaries and repeat each run under similar machine load.

On a single-configuration generator, set `-DCMAKE_BUILD_TYPE=Release` for each
separate build directory, build it, and run the resulting
`virtio_sound_core_benchmark` binary with the same iteration count. On Windows,
also pass `-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=<include-directory>`; that
directory must contain `wil/resource.h`.

## What the benchmark measures

The harness configures a synthetic 48 kHz stereo S16_LE stream and pumps one
480-frame period per iteration against fake guest memory. The fake guest
memory is 1 MiB. Each candidate device also allocates its reusable PCM scratch buffer at
construction; it is sized to 1 MiB minus 12 bytes, the maximum transfer
payload. Device creation, stream setup, and that initial scratch allocation
happen before the timed and allocation-counted loop.

The `allocations` field counts C++ `new`/`new[]` calls observed during the
repeated `PumpCapture` loop. `elapsed_ns` covers that same loop, including the
fake guest-memory callbacks. The output also reports completed frames and
integer nanoseconds per period. Compare raw output from the same harness,
Release configuration, architecture, and iteration count. The harness does
not measure initialization allocations or all process and operating-system
allocations.

The core benchmark excludes the worker process, IPC encoding, named-pipe I/O,
overlapped-event creation, service scheduling, and WASAPI. The IPC writer keeps
wire-buffer capacity based on bounded validated frame sizes and one overlapped
event for the lifetime of its session. A separate local named-pipe regression
covers repeated exact-byte writes and safe writer reuse after timeout and
cancellation; those cases use a fresh pipe after a partial-frame result and do
not access a microphone.

These results describe the synthetic device-core loop only. They do not make
claims about end-to-end capture latency, real microphone timing, WASAPI
scheduling, or behavior inside a running WSL VM.
