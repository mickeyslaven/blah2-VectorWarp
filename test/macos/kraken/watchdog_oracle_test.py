#!/usr/bin/env python3
"""Exercise the actual watchdog timeout body with a controlled clock and driver.

Only the steady clock and sleep calls are replaced in the extracted function.
The production 120-second limit and retry control flow remain intact. A second
build removes the timeout latch and must fail the same checks.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


PRELUDE = r'''
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

std::atomic<bool> global_running{true}, reconfig_in_progress{false};
std::atomic<bool> hardware_settings_failed{false}, calibration_failed{false};
std::atomic<bool> coherence_lost{true}, recovery_in_progress{false};
std::atomic<bool> force_recalibration{false};
enum class OperatingMode { COHERENT, WIDEBAND_SCAN, INDEPENDENT };
std::atomic<OperatingMode> operating_mode{OperatingMode::COHERENT};
std::recursive_mutex calibration_mutex;
struct FFT {
    std::mutex control_mutex;
    bool fft_enabled = true, auto_disabled = false, user_override = true;
} fft_control;
enum class PhaseCompensatorState { WAITING_FOR_LAG_COMPLETION, CONVERGED };
struct Phase {
    std::mutex state_mutex;
    PhaseCompensatorState state = PhaseCompensatorState::CONVERGED;
} phase;
Phase* phase_compensation = &phase;
void reset_phase_state_locked(PhaseCompensatorState state) { phase.state = state; }
constexpr int L2_RAW_MAX = 16;
std::atomic<int> l2_raw_cap{4};
int devices = 0, recovery_calls = 0, noise_off_calls = 0;
bool noise_on = false, explicit_retry = false, forced_seen = false;

struct TestClock {
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<std::chrono::steady_clock>;
    static duration elapsed;
    static time_point now() { return time_point(elapsed); }
};
TestClock::duration TestClock::elapsed{};

void set_bias_tee_all_devices(bool enabled, int) {
    noise_on = enabled;
    if (!enabled) ++noise_off_calls;
}
// f47ed2f finishes an upstream fast-retune drag from the watchdog before
// evaluating coherence. The oracle isolates timeout/latch behavior, so model
// that new dependency as a side-effect-free driver completion.
void finish_fast_retunes(int, const char*, bool) {}
void recover_coherence(bool forced) {
    ++recovery_calls;
    forced_seen = forced_seen || forced;
    calibration_failed = false;
    recovery_in_progress = true;
    noise_on = true;
    fft_control.fft_enabled = true;
    fft_control.auto_disabled = false;
    phase.state = PhaseCompensatorState::CONVERGED;
    fft_control.user_override = true;
}
void test_sleep(std::chrono::milliseconds interval) {
    TestClock::elapsed += interval;
    // All time advances here. No thread, native process, or hardware is used.
    if (explicit_retry && TestClock::elapsed == std::chrono::seconds(121))
        force_recalibration = true;
    if (TestClock::elapsed >= std::chrono::seconds(125)) global_running = false;
}
void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
void reset(bool retry) {
    global_running = true;
    reconfig_in_progress = hardware_settings_failed = calibration_failed = false;
    coherence_lost = true;
    recovery_in_progress = force_recalibration = false;
    recovery_calls = noise_off_calls = 0;
    noise_on = forced_seen = false;
    explicit_retry = retry;
    TestClock::elapsed = TestClock::duration::zero();
    fft_control.fft_enabled = fft_control.user_override = true;
    fft_control.auto_disabled = false;
    l2_raw_cap = 4;
}
'''

CHECKS = r'''
int main() {
    try {
        reset(false);
        coherence_watchdog();
        require(recovery_calls == 1, "timeout automatically restarted recovery");
        require(calibration_failed && coherence_lost && !recovery_in_progress,
                "timeout did not retain FAILED/lost/nonrecovering state");
        require(!noise_on && noise_off_calls == 1,
                "timeout did not keep noise source disabled");
        require(phase.state == PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION,
                "timeout retained a converged phase state");
        require(!fft_control.fft_enabled && fft_control.auto_disabled &&
                !fft_control.user_override && l2_raw_cap == L2_RAW_MAX,
                "timeout did not park FFT and restore steady-state buffer cap");

        reset(true);
        coherence_watchdog();
        require(recovery_calls == 2 && forced_seen,
                "explicit retry could not re-enter recovery after timeout");
        require(!calibration_failed && recovery_in_progress && noise_on,
                "explicit retry was not dispatched to recovery");
        require(noise_off_calls == 1, "explicit retry retained the expired deadline");
        std::cout << "Native watchdog oracle PASS: timeout stays failed/noise-off; explicit retry dispatches\n";
    } catch (const std::exception& error) {
        std::cerr << "watchdog oracle: " << error.what() << '\n';
        return 1;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    args = parser.parse_args()
    text = (args.source / 'src/dsp/compensation.cpp').read_text()
    begin = text.index('void coherence_watchdog() {')
    end = text.index('// ---- Periodic calibration check ----', begin)
    body = text[begin:end]
    if body.count('timed_out = true;') != 1:
        raise RuntimeError('Expected exactly one watchdog timeout latch')
    body = body.replace('std::chrono::steady_clock', 'TestClock')
    body = body.replace('std::this_thread::sleep_for', 'test_sleep')
    compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++') or 'c++'
    with tempfile.TemporaryDirectory(prefix='vectorwarp-watchdog-') as directory:
        work = Path(directory)
        source, executable = work / 'oracle.cpp', work / 'oracle'
        for negative in (False, True):
            candidate = body.replace('timed_out = true;', '// Timeout latch intentionally removed.') if negative else body
            source.write_text(PRELUDE + candidate + CHECKS)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(executable)], check=True, timeout=30)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5)
            if negative:
                if result.returncode != 1 or 'timeout automatically restarted recovery' not in result.stderr:
                    raise RuntimeError(f'Timeout-latch negative control failed to fail as expected: {result}')
                print('Watchdog negative control PASS: missing timeout latch rejected')
            else:
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print(result.stdout.strip())


if __name__ == '__main__':
    main()
