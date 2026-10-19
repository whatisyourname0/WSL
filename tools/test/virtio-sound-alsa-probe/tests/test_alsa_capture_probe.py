import errno
import io
import json
import math
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

import alsa_capture_probe as probe
from alsa_capture_probe import CaptureDiagnostics, CaptureTimeout, XRunError, capture


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class MockBackend:
    def __init__(self, *, read_results=None, wait_result=1, clock=None, samples=(-3, 4), params=(96_000, 2_048)):
        self.read_results = list(read_results or [])
        self.wait_result = wait_result
        self.clock = clock
        self.samples = samples
        self.params = params
        self.opened_device = None
        self.params_set = False
        self.started = False
        self.closed = False
        self.read_calls = 0
        self.wait_calls = 0
        self.sample_cursor = 0

    def open(self, device):
        self.opened_device = device
        return object()

    def set_params(self, handle):
        self.params_set = True
        return 0

    def get_params(self, handle):
        return self.params

    def start(self, handle):
        self.started = True
        return 0

    def wait(self, handle, timeout_ms):
        self.wait_calls += 1
        if self.clock is not None:
            self.clock.now += timeout_ms / 1000
        return self.wait_result

    def readi(self, handle, buffer, frames):
        self.read_calls += 1
        if self.read_results:
            result = self.read_results.pop(0)
            if result < 0:
                return result
        else:
            result = frames
        for index in range(result * 2):
            buffer[index] = self.samples[(self.sample_cursor + index) % len(self.samples)]
        self.sample_cursor += result * 2
        return result

    def close(self, handle):
        self.closed = True
        return 0

    def error_text(self, code):
        return f"mock errno {-code}"


class CaptureProbeTests(unittest.TestCase):
    def test_aggregates_stereo_samples_and_closes(self):
        backend = MockBackend()

        result = capture(backend, "hw:7,2", 1, chunk_frames=2_048)

        self.assertEqual(backend.opened_device, "hw:7,2")
        self.assertTrue(backend.params_set)
        self.assertTrue(backend.started)
        self.assertTrue(backend.closed)
        self.assertEqual(result.frames, 48_000)
        self.assertEqual(result.samples, 96_000)
        self.assertEqual(result.peak, 4)
        self.assertEqual(result.nonzero_samples, 96_000)
        self.assertEqual(result.rms, round(math.sqrt(12.5), 6))

    def test_read_eagain_retries_after_bounded_wait(self):
        backend = MockBackend(read_results=[-errno.EAGAIN])

        result = capture(backend, "hw:0,0", 1, chunk_frames=48_000)

        self.assertEqual(result.frames, 48_000)
        self.assertEqual(backend.read_calls, 2)
        self.assertGreaterEqual(backend.wait_calls, 2)
        self.assertTrue(backend.closed)

    def test_epipe_is_reported_as_xrun_without_recovery_and_closes(self):
        backend = MockBackend(read_results=[-errno.EPIPE])
        telemetry = []

        with self.assertRaisesRegex(XRunError, "XRUN.*no recovery attempted"):
            capture(backend, "hw:0,0", 1, telemetry=telemetry.append)

        self.assertEqual(backend.read_calls, 1)
        self.assertEqual([event["type"] for event in telemetry], ["stream_setup"])
        self.assertTrue(backend.closed)

    def test_telemetry_tracks_channel_extrema_across_short_reads_and_window_boundaries(self):
        backend = MockBackend(
            read_results=[3, 7],
            samples=(-32_768, 32_767, 0, 0),
            params=(88_888, 1_111),
        )
        telemetry = []

        result = capture(
            backend,
            "hw:2,1",
            2,
            chunk_frames=20_000,
            telemetry=telemetry.append,
        )

        self.assertEqual(result.frames, 96_000)
        self.assertEqual(telemetry[0], {
            "type": "stream_setup",
            "rate_hz": 48_000,
            "channels": 2,
            "format": "S16_LE",
            "buffer_frames": 88_888,
            "period_frames": 1_111,
        })
        windows = [event for event in telemetry if event["type"] == "window"]
        self.assertEqual([event["frames"] for event in windows], [48_000, 48_000])
        for event in windows:
            channel_0, channel_1 = event["channels"]
            self.assertEqual(channel_0["channel"], 0)
            self.assertEqual(channel_0["samples"], 48_000)
            self.assertEqual(channel_0["negative_full_scale_samples"], 24_000)
            self.assertEqual(channel_0["positive_full_scale_samples"], 0)
            self.assertEqual(channel_0["peak"], 32_768)
            self.assertEqual(channel_0["longest_zero_run_samples"], 1)
            self.assertEqual(channel_0["dc_offset"], -16_384.0)
            self.assertEqual(channel_1["channel"], 1)
            self.assertEqual(channel_1["samples"], 48_000)
            self.assertEqual(channel_1["negative_full_scale_samples"], 0)
            self.assertEqual(channel_1["positive_full_scale_samples"], 24_000)
            self.assertEqual(channel_1["peak"], 32_767)
            self.assertEqual(channel_1["longest_zero_run_samples"], 1)
            self.assertEqual(channel_1["dc_offset"], 16_383.5)

        aggregate = telemetry[-1]
        self.assertEqual(aggregate["type"], "aggregate")
        self.assertEqual(aggregate["frames"], 96_000)
        self.assertEqual(aggregate["channels"][0]["negative_full_scale_samples"], 48_000)
        self.assertEqual(aggregate["channels"][1]["positive_full_scale_samples"], 48_000)
        self.assertEqual(aggregate["channels"][0]["rms"], round(math.sqrt(32_768**2 / 2), 6))
        self.assertEqual(aggregate["channels"][1]["rms"], round(math.sqrt(32_767**2 / 2), 6))

    def test_telemetry_zero_run_spans_reads_and_aggregate_spans_windows(self):
        backend = MockBackend(read_results=[3, 7], samples=(0, 0))
        telemetry = []

        capture(
            backend,
            "hw:0,0",
            2,
            chunk_frames=20_000,
            telemetry=telemetry.append,
        )

        windows = [event for event in telemetry if event["type"] == "window"]
        self.assertEqual(len(windows), 2)
        self.assertEqual([(event["start_frame"], event["end_frame"]) for event in windows],
                         [(0, 48_000), (48_000, 96_000)])
        for event in windows:
            self.assertEqual(
                [channel["longest_zero_run_samples"] for channel in event["channels"]],
                [48_000, 48_000],
            )
        aggregate = telemetry[-1]
        self.assertEqual(
            [channel["longest_zero_run_samples"] for channel in aggregate["channels"]],
            [96_000, 96_000],
        )

    def test_xrun_diagnostics_track_frames_elapsed_loop_gap_and_params(self):
        clock = FakeClock()
        backend = MockBackend(read_results=[24_000, -errno.EPIPE], clock=clock)
        diagnostics = CaptureDiagnostics()

        with self.assertRaises(XRunError):
            capture(backend, "hw:0,0", 1, monotonic=clock, chunk_frames=24_000, diagnostics=diagnostics)

        self.assertEqual(diagnostics.frames_read, 24_000)
        self.assertEqual(diagnostics.elapsed_seconds, 0.5)
        self.assertEqual(diagnostics.max_loop_gap_ms, 250.0)
        self.assertEqual(diagnostics.buffer_frames, 96_000)
        self.assertEqual(diagnostics.period_frames, 2_048)
        self.assertTrue(diagnostics.stream_started)
        self.assertTrue(backend.closed)

    def test_main_emits_failure_diagnostics_only_to_stderr(self):
        backend = MockBackend(read_results=[1_024, -errno.EPIPE])
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patch.object(probe, "NativeAlsaBackend", return_value=backend):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                exit_code = probe.main(
                    ["--device", "hw:0,0", "--duration", "1", "--acknowledge-microphone-access"]
                )

        self.assertEqual(exit_code, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("ALSA XRUN during snd_pcm_readi", stderr.getvalue())
        diagnostic_line = next(
            line for line in stderr.getvalue().splitlines() if line.startswith("alsa_capture_probe_diagnostics: ")
        )
        payload = json.loads(diagnostic_line.split(": ", 1)[1])
        self.assertEqual(
            set(payload),
            {"frames_read", "elapsed_seconds", "max_loop_gap_ms", "buffer_frames", "period_frames"},
        )
        self.assertEqual(payload["frames_read"], 1_024)
        self.assertEqual(payload["buffer_frames"], 96_000)
        self.assertEqual(payload["period_frames"], 2_048)

    def test_main_success_json_fields_remain_unchanged(self):
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patch.object(probe, "NativeAlsaBackend", return_value=MockBackend()):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                exit_code = probe.main(
                    ["--device", "hw:0,0", "--duration", "1", "--acknowledge-microphone-access"]
                )

        payload = json.loads(stdout.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(set(payload), {"frames", "samples", "rms", "peak", "nonzero_samples"})
        self.assertEqual(stderr.getvalue(), "")

    def test_main_telemetry_keeps_success_json_unchanged_and_streams_to_stderr(self):
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patch.object(probe, "NativeAlsaBackend", return_value=MockBackend()):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                exit_code = probe.main(
                    [
                        "--device", "hw:0,0", "--duration", "1",
                        "--acknowledge-microphone-access", "--telemetry",
                    ]
                )

        payload = json.loads(stdout.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(set(payload), {"frames", "samples", "rms", "peak", "nonzero_samples"})
        lines = stderr.getvalue().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertTrue(all(line.startswith("alsa_capture_probe_telemetry: ") for line in lines))
        events = [json.loads(line.split(": ", 1)[1]) for line in lines]
        self.assertEqual([event["type"] for event in events], ["stream_setup", "window", "aggregate"])
        self.assertEqual(events[0]["buffer_frames"], 96_000)
        self.assertEqual(events[0]["period_frames"], 2_048)

    def test_wait_timeout_hits_monotonic_deadline_and_closes(self):
        clock = FakeClock()
        backend = MockBackend(wait_result=0, clock=clock)

        with self.assertRaises(CaptureTimeout):
            capture(backend, "hw:0,0", 1, monotonic=clock)

        self.assertGreaterEqual(clock.now, 11.0)
        self.assertTrue(backend.closed)


if __name__ == "__main__":
    unittest.main()
