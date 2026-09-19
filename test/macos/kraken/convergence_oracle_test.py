#!/usr/bin/env python3
"""Exercise the actual native convergence decision with controlled measurements.

No receiver library or USB device is used. The compiled source body is copied
verbatim from the patched native implementation; doubles record its hardware
side effects. This makes exhaustion of the retry budget deterministic.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Heimdall_v2 source directory')
    args = parser.parse_args()
    source = (args.source / 'src/dsp/compensation.cpp').read_text()
    begin = source.index('bool check_phase_convergence(')
    end = source.index('std::optional<PhaseCompensatorState> get_phase_compensation_state()', begin)
    body = source[begin:end]
    prelude = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
using Complex = std::complex<float>;
constexpr int REF_CHANNEL = 0;
enum class OperatingMode { COHERENT, WIDEBAND_SCAN };
enum class PhaseCompensatorState { VERIFYING, WAITING_FOR_LAG_COMPLETION, CONVERGED };
struct Vector {
  std::map<int, Complex> values;
  Complex load(int ch) { return values.count(ch) ? values[ch] : Complex(1,0); }
  void store(int ch, Complex value) { values[ch] = value; }
};
struct Phase {
  std::atomic<bool> convergence_check_active{false};
  std::mutex state_mutex;
  int checks_since_compensation=0, convergence_count=0, failed_convergence_attempts=0;
  int required_convergence_readings=4, max_checks_before_recompensate=10, max_convergence_attempts=3;
  float convergence_threshold_degrees=1.0f, amplitude_tolerance_db=2.0f;
  PhaseCompensatorState state=PhaseCompensatorState::VERIFYING;
  Vector compensation_vector;
};
std::unique_ptr<Phase> phase_compensation;
std::recursive_mutex calibration_mutex;
std::atomic<OperatingMode> operating_mode{OperatingMode::COHERENT};
std::atomic<bool> calibration_failed{false}, recovery_in_progress{true};
struct FFT { std::mutex control_mutex; bool fft_enabled=true, auto_disabled=false, user_override=true; } fft_control;
std::vector<int> devices{0,1,2,3,4};
int converged=0, flushes=0, noise_off=0;
void proceed_after_scalar_convergence_locked() { ++converged; phase_compensation->state=PhaseCompensatorState::CONVERGED; }
void clear_l2_buffer() { ++flushes; }
void set_bias_tee_all_devices(bool enable, const std::vector<int>&) {
  if (enable) throw std::runtime_error("exhausted convergence turned noise on");
  ++noise_off;
}
void require(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
void reset() {
  phase_compensation=std::make_unique<Phase>(); calibration_failed=false; recovery_in_progress=true;
  converged=flushes=noise_off=0; fft_control.fft_enabled=true; fft_control.auto_disabled=false;
}
'''
    checks = r'''
int main() {
  for (int mode=0; mode<2; ++mode) {
    reset();
    std::map<int,float> phases{{0,0},{1,mode ? 0.0f : 1.01f},{2,0},{3,0},{4,0}};
    std::map<int,float> amplitudes{{0,1},{1,mode ? std::pow(10.0f,2.01f/20.0f) : 1.0f},{2,1},{3,1},{4,1}};
    const int budget=phase_compensation->max_checks_before_recompensate*phase_compensation->max_convergence_attempts;
    for (int i=0; i<budget; ++i) require(!check_phase_convergence(phases,amplitudes), "failed residual declared converged");
    require(converged==0 && calibration_failed.load(), "exhausted retry must latch calibration failure");
    require(noise_off==1 && !fft_control.fft_enabled && !recovery_in_progress.load(), "failure must disable noise and finish recovery");
    require(phase_compensation->state!=PhaseCompensatorState::CONVERGED && !phase_compensation->convergence_check_active.load(), "failure left a converged or active state");
    require(flushes==phase_compensation->max_convergence_attempts-1, "retry budget was not exercised");
  }
  reset();
  std::map<int,float> phases{{0,0},{1,1.0f},{2,-1.0f},{3,0},{4,0}};
  std::map<int,float> amplitudes{{0,1},{1,std::pow(10.0f,1.9f/20.0f)},{2,1},{3,1},{4,1}};
  for(int i=0;i<phase_compensation->required_convergence_readings-1;++i)
    require(!check_phase_convergence(phases,amplitudes), "converged before required readings");
  require(check_phase_convergence(phases,amplitudes) && converged==1 && !calibration_failed.load(), "valid threshold measurements failed");
  std::cout << "Native convergence oracle PASS: phase/amplitude retry exhaustion stays failed; valid boundary converges\n";
}
'''
    with tempfile.TemporaryDirectory(prefix='vectorwarp-convergence-') as directory:
        path = Path(directory)
        (path / 'oracle.cpp').write_text(prelude + body + checks)
        subprocess.run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(path / 'oracle.cpp'), '-o', str(path / 'oracle')], check=True)
        subprocess.run([str(path / 'oracle')], check=True)


if __name__ == '__main__':
    main()
