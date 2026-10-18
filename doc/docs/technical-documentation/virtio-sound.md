# Experimental virtio-sound capture

The experimental Windows host backend uses Linux's existing `virtio_snd` driver.
Its intended result is an ALSA capture card in `/dev/snd` that an application or
container can open through the ordinary Linux ALSA interface. The implementation
is experimental and disabled at build time and at runtime by default.

## Configuration and kernel dependency

Build WSL using the [developer instructions](../dev-loop.md), with
`-DWSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND=ON` supplied to CMake. The normal build
leaves this option off. The experimental package includes `wsl-audio-worker.exe`.
The guest kernel must contain `CONFIG_SND_VIRTIO=m` and its ALSA dependencies,
with modules built for the exact running kernel. Enabling the host option does
not install or replace a guest kernel.

The following experimental settings select the deterministic synthetic source:

```ini
[experimental]
virtioSound=true
virtioSoundWindowsInput=false
```

Safe mode and disabling virtio disable the device, clear the physical-input
request, and clear its endpoint. When the device remains enabled,
configuration adds `virtio_snd` to the guest module list. Both the device and the
driver must initialize successfully before `/dev/snd` exists. Container device
mapping alone cannot create the host device.

Physical capture additionally requires `virtioSoundWindowsInput=true` and an
explicit `virtioSoundCaptureEndpoint` Windows capture endpoint identifier. It
does not silently choose a default microphone. Do not include real endpoint
identifiers or captured PCM in public diagnostic logs. The physical path remains
subject to separate runtime acceptance and should be exercised only in an opted-in development
environment. Guest PCM START causes microphone access; selecting the feature is
not a per-application permission prompt.

## Process and data path

`WslCoreVm` describes the external FlexibleIov device in the VM creation schema
and starts a VM-owned child of the service before starting the VM. The child
hosts the HDV callbacks, the virtio PCI device, and the control/event/TX/RX
virtqueues. The default source generates a deterministic 48 kHz, stereo S16_LE
ramp. This tests the actual guest driver and device path without an endpoint.

For physical input, the child starts an authenticated worker in the originating
user session using a restricted user token. The worker opens the explicit WASAPI
endpoint and sends validated PCM/control messages over a local pipe. The host
checks stream epoch/generation and copies complete frames into a bounded ring.
Guest DMA consumes only available hardware frames. Synthetic capture uses a
monotonic elapsed-time frame budget instead.

The core retains a PCM START control response while worker readiness is pending.
It acknowledges START only after a validated current-generation Started message;
startup failure returns an I/O error. Reset or a guest-memory fault invalidates
the retained chain. A 750 ms worker-start deadline is intended to fit within the
guest control timeout; scheduling and controller I/O are not hard-real-time.

The first valid retained WASAPI packet establishes the stream baseline. Later
discontinuities, timestamp errors, and ring overflow fail the stream. This policy
does not promise lossless data before that baseline, automatic reconnection,
arbitrary formats, playback, or long-running stability.

## Lifetime and security review

The service confirms guest shutdown before signalling graceful host retirement.
Parent death and unconfirmed guest exit use the failure/job-abort path. HDV
callback state remains rooted until child exit when ownership of the native host
registration cannot be proven; this avoids tearing down another registration.

The current HDV child retains SYSTEM identity at High integrity. A separate
process and kill-on-close job are lifetime boundaries, not a privilege sandbox
for guest-controlled descriptors. Minimum privileges, coexistence with WSL's
existing device host, and supported HCS/HDV ownership semantics need maintainer
and security review before enabling this feature in a shipping build. The WASAPI
worker's restricted user token does not remove the HDV child's review requirement.

## Offline tests and runtime validation

From the repository root, run the standalone suite with CMake, Visual Studio
C++ tools, and a Windows SDK installed:

```powershell
cmake -S src/windows/virtiosound -B out/virtio-sound-tests -G "Visual Studio 17 2022" -A x64 -DWSL_VIRTIO_SOUND_BUILD_TESTS=ON
cmake --build out/virtio-sound-tests --config Debug --parallel 1
ctest --test-dir out/virtio-sound-tests -C Debug --output-on-failure
```

The tests use fake guest memory, device callbacks,
and capture sources; they do not launch WSL, create a VM, or open an audio endpoint.
Windows token tests inspect only the current test process's token.

Standalone Windows builds can fetch the pinned WIL headers. To use existing
headers without downloading them, pass
`-DWSL_VIRTIO_SOUND_WIL_INCLUDE_DIR=<directory-containing-wil>`.
The offline GitHub workflow runs this suite, while the WSL PR pipeline builds
the opt-in backend for x64 and ARM64. TAEF configuration tests participate in
the repository's normal Windows test suite. CI compilation does not enable the
device or open a microphone in a guest.

Guest validation must separately check PCI/virtio enumeration, module binding,
ALSA card/PCM metadata, exact synthetic ramp samples, and repeated stream
start/stop. A container test should open the same guest card with only `/dev/snd`
mapped. Physical validation must additionally exercise the real worker startup,
selected endpoint, device loss, clock pacing, underrun, and cleanup.

The standalone [ALSA capture probe](../../../tools/test/virtio-sound-alsa-probe/README.md)
is an independent example for reading an explicitly selected guest ALSA device.
Its mock checks do not validate physical capture or device mapping under WSL;
those remain pending.

Record results against the exact source revision, WSL package version, matched
kernel/module pair, and input backend. Earlier binary results are not runtime
validation of changed source. WSL package version numbers and the WSL1/WSL2
architecture designation are separate version axes.

Source-clocked capture rejects PCM buffer/period configurations that cannot
sustain a conservative 10 ms service interval. The per-pump budget retains one
guest period of headroom and must contain at least 10 ms of frames in complete
periods. At 48 kHz, a 512-frame buffer with a 128-frame period is unsupported;
a 1024-frame buffer with a 256-frame period satisfies this capacity check.
This is a throughput constraint, not a guarantee against host scheduling stalls.

## Capture scheduling and performance diagnostics

The HDV child waits on stop, parent exit, and a coalescing capture event. Source
data/readiness/failure and guest RX/state changes signal that event. RX queue
notifications remain enabled; HDV callbacks signal the owner and never transfer
PCM themselves. A shared event owner remains alive with pinned callback state.
The core services pending START and failure transitions even when PCM transfer
is deferred. Source-clocked capture waits indefinitely when idle, waiting for
readiness, missing a source period, or missing a guest RX buffer.

Each service transfers at most one negotiated period. An explicit completion
deadline prevents repeated events from granting extra sample credit. Sub-period
lateness advances the previous absolute deadline to avoid cumulative scheduling
drift; a delay of a full period rebases the phase rather than replaying all missed
slots. Runnable backlog arms one deadline; synthetic input additionally uses
deadlines to advance its sample clock. This is a bounded completion-rate policy,
not proof that an ALSA application consumed the previous period. The conservative
10 ms configuration capacity check remains unchanged. Scheduling stalls can
still overflow bounded queues and must surface as errors, not dropped audio.

The worker session and source controller each own one overlapped reader and
writer, and wait on their completion events alongside state/process events.
Readers have no idle timeout. The partial-frame deadline starts when `Advance`
observes completion of the first header read and covers the rest of that frame. Writes have a bounded deadline, and pending IO is canceled and
drained before closing a pipe or releasing its buffers. Control transitions get
an opportunity between received frames, including under continuous PCM input.
The worker preserves response order in a bounded queue and drains Closed/Fault
before normal exit. Authentication, restricted tokens, jobs, and frame limits
remain in place. Bootstrap connection/handshake waits are separate from this
steady-state loop.

WASAPI waits on capture and stop events. A 1-second endpoint-health deadline
rechecks device state if capture callbacks stop; it is not a transfer cadence.
The controller retains its 1-second session-health check and lifecycle deadlines.
No shared memory, zero-copy, IOCP, or interrupt coalescing is introduced here.

`DiagnosticSnapshot` exposes device-lifetime saturating totals for pump calls,
requested/completed frames, calls without a successful completion, elapsed call
count and nanoseconds, maximum interval, and observed source/RX backlog. Scratch
capacity and growth count show the retained allocation cost. These counters
survive guest device reset and contain no PCM, endpoint identifiers, or per-period
records. Backlog values are maxima observed at existing queue reads, not a full
history of all arrivals. Averages must use matching counters: elapsed totals
count all elapsed-pump calls, while frame totals count calls reaching the core
pump, including direct test calls.

After callbacks and pumps drain, guest retirement emits aggregate `CapturePump*`,
`CaptureInterval*`, `CaptureBacklogMax`, and `CaptureScratch` events through the
existing AudioSource ETW provider. Normal child shutdown additionally emits
`CaptureWakeTotals` (event/deadline wakes), `CaptureWakeReasons` (source/guest),
`CaptureServiceEmpty` (empty/deferred services), `CaptureServiceTime` (total/peak
nanoseconds), and `CaptureServiceFrames`. Controller cleanup emits
`CaptureController*` aggregates for state/IO wakes, worker exits, message/byte
counts, busy writes, write timeouts, and peak write duration. The worker snapshot
counts queue activity signals; these can coalesce and are not wake counts.
Forced process exit does not guarantee a summary. No per-period ETW event is
added by this instrumentation.

The core allocates its bounded PCM scratch vector (1 MiB minus 12 bytes of
transport overhead) on the first valid PCM configuration, before capture can
start, and retains it across resets and later configurations. The worker retains its validated IPC wire buffer
and one overlapped event per writer. Both buffers can retain previous PCM in
private process memory until overwritten or destroyed; diagnostics never export
them. Pipe cancellation drains outstanding I/O before reuse. A failed write can
leave a partial wire frame, so the pipe session must be discarded even though
the writer's buffer and event can be reused with a fresh pipe.

The [comparison benchmark](../../../src/windows/virtiosound/benchmarks/README.md)
builds one harness against baseline and candidate core sources. Its allocation
counts and the deterministic scheduler simulation do not measure microphone
latency, host CPU consumption, or timer behavior under VM load. The event
scheduler tests also cover repeated late wakes, long stalls, duplicate events,
idle waits, and synthetic deadlines. Local byte-pipe tests cover partial frames,
duplex progress, timeout, cancellation, and reuse. These checks do not exercise
the complete authenticated process graph or physical endpoint loss. Validate
those separately before claiming end-to-end performance gains.

## RX doorbell shutdown ordering

Normal VM shutdown requests RX doorbell preparation before closing the mini_init
channel. The private HDV child unregisters the optional doorbell from its pump
thread, outside callbacks, and acknowledges the result. Stop preparation drains
in-flight registration and prevents new registration; MMIO notification fallback
remains available during guest shutdown. Post-exit retirement does not retry
unregister against a stopped device.

If preparation times out or its wait fails, the parent clears the pending request
and closes only its owned kill-on-close child job. The request wait and emergency
exit wait are each bounded to two seconds, though OS scheduling can extend elapsed
time. Emergency exit is not reported as successful unregister. Uncertain event
ownership remains pinned until the private process exits, and shared or unknown
HDV host ownership never permits whole-host teardown.

The ordering guarantee applies to orderly stop. An independent guest exit can
race a preparation already in flight; unregister may then return an invalid-state
error. Failure retains the event/callback root and is negatively acknowledged,
rather than being treated as successful unregister. No cross-process state check
can make that independent exit atomic with an HDV API call.
