#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuLaneLease.h"
#include "process/analysis/StartupIq.h"
#include <cmath>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>

using Complex = std::complex<double>;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct State {
  std::string fault;
  unsigned calls = 0, clutterCalls = 0;
  std::vector<std::complex<float>> output;
  std::vector<std::complex<float>> expectedReference, expectedSurveillance;
};
struct Backend : blah2::GpuBackend {
  std::shared_ptr<State> state;
  explicit Backend(std::shared_ptr<State> value) : state(std::move(value)) {}
  blah2::GpuDevice device() const override { return {"test", "Test double", 0}; }
  void process(const std::vector<std::complex<float>>&, const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>& output) override {
    ++state->calls;
    if (state->fault == "throw" || (state->fault == "later" && state->calls > 3))
      throw std::runtime_error("GPU device lost; using CPU");
    if (state->fault == "transient" && state->calls > 3)
      throw std::runtime_error("GPU worker stopped unexpectedly; using CPU");
    output = state->output;
    if (state->fault == "wrong") for (auto& item : output) item *= 2;
    if (state->fault == "nan") output[0] = {std::numeric_limits<float>::quiet_NaN(), 0};
    // Preserve the retired full-map guard's unique tail coverage in the
    // canonical accelerator: a prefix-only oracle must not accept these.
    if (state->fault == "nan-tail") output.back() = {std::numeric_limits<float>::quiet_NaN(), 0};
    if (state->fault == "wrong-tail") output.back() += std::complex<float>(1e6f, 0);
    if (state->fault == "shape") output.clear();
  }
};
struct FrameBackend : blah2::GpuBackend, blah2::GpuFrameBackend {
  std::shared_ptr<State> state;
  std::vector<std::complex<float>> reference, surveillance, output;
  FrameBackend(std::shared_ptr<State> value, const blah2::GpuGeometry& geometry)
    : state(std::move(value)),
      reference(size_t(geometry.range) * geometry.doppler),
      surveillance(reference.size() * geometry.channels),
      output(size_t(geometry.doppler) * geometry.delays * geometry.channels) {}
  blah2::GpuDevice device() const override { return {"frame-test", "Shared-frame test double", 0}; }
  void process(const std::vector<std::complex<float>>&,
      const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>&) override {
    throw std::runtime_error("Legacy vector path used for shared-frame backend");
  }
  blah2::GpuFrameBuffers frameBuffers() override {
    // Poison reusable storage so a missing sample/padding write cannot pass
    // just because the first allocation happened to contain zeros.
    std::fill(reference.begin(), reference.end(), std::complex<float>{123, -456});
    std::fill(surveillance.begin(), surveillance.end(), std::complex<float>{789, -123});
    return {reference.data(), reference.size(), surveillance.data(), surveillance.size(),
      output.data(), output.size()};
  }
  void processFrame() override {
    if (!state->expectedReference.empty()) {
      check(reference == state->expectedReference, "Packed reference or FFT padding changed");
      check(surveillance == state->expectedSurveillance, "Packed channel order or FFT padding changed");
    }
    ++state->calls; output = state->output;
  }
};
struct CombinedBackend : blah2::GpuBackend, blah2::GpuClutterFrameBackend {
  blah2::GpuGeometry geometry;
  std::shared_ptr<State> state;
  std::vector<std::complex<float>> clutterReference, clutterSurveillance, clutterOutput;
  CombinedBackend(blah2::GpuGeometry value, std::shared_ptr<State> shared) : geometry(value), state(std::move(shared)),
    clutterReference(value.clutterSamples),
    clutterSurveillance(size_t(value.clutterSamples)*value.channels),
    clutterOutput(clutterSurveillance.size()) {}
  blah2::GpuDevice device() const override { return {"combined", "Combined test double", 1}; }
  void process(const std::vector<std::complex<float>>&,
      const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>& output) override {
    ++state->calls;
    if(state->fault=="ambiguity-transient")
      throw std::runtime_error("GPU worker stopped unexpectedly; using CPU");
    if(state->fault=="ambiguity-throw") throw std::runtime_error("Injected ambiguity worker failure");
    output=state->output;
    if(state->fault=="wrong") for(auto& value:output) value*=2;
    if(state->fault=="ambiguity-nan") output[0]={std::numeric_limits<float>::quiet_NaN(),0};
    if(state->fault=="ambiguity-shape") output.clear();
  }
  bool clutterAvailable() const override { return true; }
  blah2::GpuClutterBuffers clutterBuffers() override {
    return {clutterReference.data(),clutterReference.size(),
      clutterSurveillance.data(),clutterSurveillance.size(),
      clutterOutput.data(),clutterOutput.size()};
  }
  bool processClutterFrame() override {
    ++state->clutterCalls;
    if(state->fault=="clutter-throw") throw std::runtime_error("Injected clutter worker failure");
    if(state->fault=="clutter-reject") return false;
    std::fill(clutterOutput.begin(),clutterOutput.end(),std::complex<float>{});
    if(state->fault=="clutter-nan") clutterOutput[0]={std::numeric_limits<float>::quiet_NaN(),0};
    return true;
  }
};
// Deterministic ordering oracle: host reference work must run between submission
// and completion, exactly once across rejection/failure and before CPU mutation.
void testClutterHostWork() {
  struct OverlapBackend : CombinedBackend, blah2::GpuClutterHostWorkBackend {
    std::vector<std::string>& events;
    OverlapBackend(blah2::GpuGeometry geometry, std::shared_ptr<State> state,
        std::vector<std::string>& order)
      : CombinedBackend(geometry, std::move(state)), events(order) {}
    bool processClutterFrameWithHostWork(const std::function<void()>& hostWork) override {
      events.push_back("submitted");
      if (state->fault == "before-host") throw std::runtime_error("Injected submission failure");
      hostWork();
      events.push_back("completed");
      return processClutterFrame();
    }
  };
  const blah2::GpuGeometry geometry{8, 4, 3, 1, 0, 32, 1, 0};
  for (const std::string fault : {"", "cpu", "legacy", "before-host", "clutter-reject",
      "clutter-throw", "host-throw"}) {
    auto state = std::make_shared<State>(); state->fault = fault;
    std::vector<std::string> events;
    auto factory = [&](const blah2::GpuGeometry& shape, const std::string&)
        -> std::unique_ptr<blah2::GpuBackend> {
      std::unique_ptr<blah2::GpuBackend> backend;
      if (fault == "legacy") backend = std::make_unique<CombinedBackend>(shape, state);
      else backend = std::make_unique<OverlapBackend>(shape, state, events);
      return blah2::leaseGpuLane(std::move(backend),
        std::make_shared<blah2::resident::LaneBudget>(1));
    };
    blah2::Acceleration acceleration(fault == "cpu" ? "cpu" : "auto",
      geometry, 8, 64, 0, "auto", factory);
    IqData reference(32), input(32);
    const std::deque<Complex> original(32, Complex(1, .5));
    reference.replace(std::deque<Complex>(original));
    input.replace(std::deque<Complex>(original));
    unsigned hostCalls = 0, cpuCalls = 0;
    bool threw = false;
    try {
      check(acceleration.processClutter(reference, {&input}, [&] {
        check(hostCalls == 1, "CPU clutter ran before host reference work");
        ++cpuCalls; events.push_back("cpu"); return true;
      }, [&] {
        ++hostCalls; events.push_back("host");
        check(input.view_data() == original && reference.view_data() == original,
          "Host reference work observed committed clutter");
        if (fault == "host-throw") throw std::runtime_error("Injected host reference failure");
      }), "Clutter host-work fixture rejected");
    } catch (const std::runtime_error& error) {
      threw = true;
      check(fault == "host-throw" && std::string(error.what()) == "Injected host reference failure",
        "Host reference exception was swallowed or relabelled");
    }
    check(hostCalls == 1 && threw == (fault == "host-throw"),
      "Host reference work was skipped/retried");
    check(input.view_data() == original, "Failed or zero clutter changed input");
    if (fault == "host-throw") {
      check(cpuCalls == 0 && state->clutterCalls == 1 && acceleration.available(),
        "Host failure retried CPU or retired a drained healthy worker");
      check(events == std::vector<std::string>{"submitted", "host", "completed"},
        "Host exception escaped before worker completion");
      state->fault.clear();
      check(acceleration.processClutter(reference, {&input}, [&] {
        ++cpuCalls; return true;
      }, [&] { ++hostCalls; }), "Drained worker could not process the next frame");
      check(cpuCalls == 1 && hostCalls == 2 && state->clutterCalls == 2,
        "Host failure left a stale callback or repeated the previous frame");
    } else {
      check(cpuCalls == 1, "First-frame qualification/fallback count changed");
      if (fault != "cpu" && fault != "legacy" && fault != "before-host")
        check(events == std::vector<std::string>{"submitted", "host", "completed", "cpu"},
          "Clutter overlap ordering differs");
    }
  }
}
void testIndependentPortableLanesCanOverlap() {
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0, active = 0, maximum = 0;
  } gate;
  struct OverlapBackend final : blah2::GpuBackend {
    Gate& gate;
    explicit OverlapBackend(Gate& value) : gate(value) {}
    blah2::GpuDevice device() const override { return {"overlap", "Synthetic lane", 0}; }
    void process(const std::vector<std::complex<float>>&,
        const std::vector<std::complex<float>>&,
        std::vector<std::complex<float>>&) override {
      std::unique_lock<std::mutex> lock(gate.mutex);
      ++gate.entered; ++gate.active;
      gate.maximum = std::max(gate.maximum, gate.active);
      gate.changed.notify_all();
      gate.changed.wait_for(lock, std::chrono::milliseconds(500),
        [&] { return gate.entered == 2; });
      --gate.active;
    }
  };
  auto budget = std::make_shared<blah2::resident::LaneBudget>(2);
  auto first = blah2::leaseGpuLane(std::make_unique<OverlapBackend>(gate), budget);
  auto second = blah2::leaseGpuLane(std::make_unique<OverlapBackend>(gate), budget);
  const std::vector<std::complex<float>> input;
  std::vector<std::complex<float>> firstOutput, secondOutput;
  std::thread left([&] { first->process(input, input, firstOutput); });
  std::thread right([&] { second->process(input, input, secondOutput); });
  left.join(); right.join();
  check(gate.maximum == 2,
    "Independent portable lanes were serialized despite separate worker transactions");
}

void testPackedFrames() {
  constexpr unsigned samples = 4800, fs = 48000;
  for (unsigned channels : {1, 4, 8}) for (int middle : {0, 110, -110}) {
    std::vector<std::unique_ptr<Ambiguity>> processors;
    std::vector<std::unique_ptr<IqData>> storage;
    std::vector<Ambiguity*> cpu;
    std::vector<IqData*> inputs;
    for (unsigned channel = 0; channel < channels; ++channel) {
      processors.push_back(std::make_unique<Ambiguity>(-5, 20,
        middle - 100, middle + 100, fs, samples, true));
      storage.push_back(std::make_unique<IqData>(samples));
      cpu.push_back(processors.back().get()); inputs.push_back(storage.back().get());
    }
    auto state = std::make_shared<State>();
    auto factory = [state](const blah2::GpuGeometry& geometry, const std::string&)
      -> std::unique_ptr<blah2::GpuBackend> { return std::make_unique<FrameBackend>(state, geometry); };
    const unsigned range = cpu[0]->get_nfft(), doppler = cpu[0]->get_n_doppler_bins();
    const unsigned correlation = cpu[0]->get_n_corr(), delays = cpu[0]->get_n_delay_bins();
    blah2::Acceleration acceleration("gpu", {range, doppler, delays, channels, -5},
      correlation, fs, middle, "auto", factory);
    unsigned cpuCalls = 0;
    for (unsigned frame = 0; frame < 5; ++frame) {
      std::deque<Complex> reference;
      for (unsigned i = 0; i < samples; ++i)
        reference.emplace_back(std::sin(i * .37 + frame), std::cos(i * .61 - frame));
      std::vector<std::deque<Complex>> raw(channels);
      state->output.resize(size_t(doppler) * delays * channels);
      for (unsigned channel = 0; channel < channels; ++channel) {
        for (unsigned i = 0; i < samples; ++i)
          raw[channel].emplace_back(std::sin(i * .41 + channel + frame),
            std::cos(i * .17 - channel - frame));
        storage[channel]->replace(std::deque<Complex>(raw[channel]));
        cpu[channel]->process(reference, inputs[channel]);
        for (unsigned d = 0; d < doppler; ++d) for (unsigned r = 0; r < delays; ++r)
          state->output[(size_t(channel) * delays + r) * doppler +
            (d + doppler / 2 + 1) % doppler] = cpu[channel]->result()->data[d][r];
        storage[channel]->replace(std::deque<Complex>(raw[channel]));
      }
      state->expectedReference.assign(size_t(range) * doppler, {});
      state->expectedSurveillance.assign(size_t(range) * doppler * channels, {});
      // Independent scalar producer matching the former channel-interleaved path.
      for (unsigned d = 0; d < doppler; ++d) for (unsigned r = 0; r < correlation; ++r) {
        const size_t sample = size_t(d) * correlation + r;
        const size_t target = size_t(d) * range + r;
        auto value = reference[sample];
        if (middle != 0) value *= std::polar(1.0, 2 * std::acos(-1.0) * middle * sample / fs);
        state->expectedReference[target] = value;
        for (unsigned channel = 0; channel < channels; ++channel)
          state->expectedSurveillance[size_t(channel) * range * doppler + target] = raw[channel][sample];
      }
      acceleration.process(reference, inputs, cpu, [&] {
        ++cpuCalls;
        for (unsigned channel = 0; channel < channels; ++channel)
          cpu[channel]->process(reference, inputs[channel]);
      });
      check(acceleration.status().state != "fallback", "Shared-frame producer failed qualification");
      for (unsigned channel = 0; channel < channels; ++channel) {
        const auto& tail = storage[channel]->view_data();
        check(tail == std::deque<Complex>(raw[channel].begin() + correlation * doppler,
          raw[channel].end()), "Bulk retirement changed the unprocessed IQ tail");
      }
    }
    check(cpuCalls == 3 && state->calls == 5, "Packed frames repeated CPU work after qualification");
  }
  std::cout << "PASS packed shared frames: channels=1/4/8, positive/zero/negative offset\n";
}
void testStartupPreparation() {
  constexpr unsigned samples = 4800;
  Ambiguity cpu(-5, 20, -100, 100, 48000, samples, true);
  IqData reference(samples), input(samples);
  const unsigned doppler = cpu.get_n_doppler_bins(), delays = cpu.get_n_delay_bins();
  for (bool borrowed : {false, true})
  for (const std::string fault : {"", "wrong", "throw"}) {
    auto state = std::make_shared<State>(); state->fault = fault;
    const auto factory = [state](const blah2::GpuGeometry&, const std::string&)
        -> std::unique_ptr<blah2::GpuBackend> { return std::make_unique<Backend>(state); };
    blah2::Acceleration acceleration("gpu", {cpu.get_nfft(), doppler, delays, 1, -5},
      cpu.get_n_corr(), 48000, 0, "auto", factory);
    unsigned calls = 0;
    bool producerStarted = false;
    const auto process = [&] {
      const auto cpuRun = [&] { ++calls; cpu.process(reference.view_data(), &input); };
      if (borrowed) {
        const std::vector<Complex> ref(reference.view_data().begin(), reference.view_data().end());
        const std::vector<Complex> surv(input.view_data().begin(), input.view_data().end());
        acceleration.processBorrowed(ref.data(), {surv.data()}, samples, 0, {&cpu}, cpuRun);
      } else {
        acceleration.process(reference.view_data(), {&input}, {&cpu}, cpuRun);
      }
    };
    acceleration.prepareStartup([&](unsigned frame) {
      check(!producerStarted, "Producer admitted during startup qualification");
      blah2::startup::fillIq({&reference, &input}, samples, frame);
      const auto raw = input.get_data();
      cpu.process(reference.view_data(), &input);
      state->output.resize(size_t(doppler) * delays);
      for (unsigned d = 0; d < doppler; ++d) for (unsigned r = 0; r < delays; ++r)
        state->output[r * doppler + (d + doppler / 2 + 1) % doppler] =
          cpu.result()->data[d][r];
      input.replace(std::deque<Complex>(raw));
      process();
    });
    check(!acceleration.startupPending(), "Startup returned while qualification pending");
    check(calls == (fault.empty() ? 3u : 1u), "Startup bypassed CPU numerical checks");
    check(acceleration.status().active == (fault.empty() ? "vulkan" : "cpu"),
      "Startup numerical failure did not preserve CPU fallback");
    acceleration.prepareStartup([&](unsigned) {
      throw std::runtime_error("Completed startup qualification was repeated");
    });
    producerStarted = true;
    if (fault.empty()) {
      blah2::startup::fillIq({&reference, &input}, samples, 2);
      process();
      check(calls == 3, "First admitted frame repeated startup CPU qualification");
    }
  }
  auto state = std::make_shared<State>();
  blah2::Acceleration stuck("gpu", {cpu.get_nfft(), doppler, delays, 1, -5},
    cpu.get_n_corr(), 48000, 0, "auto",
    [state](const blah2::GpuGeometry&, const std::string&) {
      return std::make_unique<Backend>(state);
    });
  unsigned attempts = 0;
  bool rejected = false;
  try { stuck.prepareStartup([&](unsigned) { ++attempts; }); }
  catch (const std::runtime_error&) { rejected = true; }
  check(rejected && attempts == blah2::Acceleration::TotalQualificationFrames &&
    stuck.startupPending(), "Stalled startup did not fail closed at its bound");
  std::cout << "PASS pre-capture qualification, fallback, steady admission and bound\n";
}

void testTransientWorkerRecovery() {
  constexpr unsigned samples = 4800;
  Ambiguity cpu(-5, 20, -100, 100, 48000, samples, true);
  IqData reference(samples), input(samples);
  const unsigned doppler = cpu.get_n_doppler_bins(), delays = cpu.get_n_delay_bins();
  auto failed = std::make_shared<State>(); failed->fault = "transient";
  auto replacement = std::make_shared<State>();
  unsigned creations = 0, cpuCalls = 0;
  blah2::Acceleration acceleration("gpu", {cpu.get_nfft(), doppler, delays, 1, -5},
    cpu.get_n_corr(), 48000, 0, "auto",
    [&](const blah2::GpuGeometry&, const std::string&) -> std::unique_ptr<blah2::GpuBackend> {
      ++creations;
      return std::make_unique<Backend>(creations == 1 ? failed : replacement);
    });
  for (unsigned frame = 0; frame < 6; ++frame) {
    blah2::startup::fillIq({&reference, &input}, samples, frame);
    const auto raw = input.get_data();
    cpu.process(reference.view_data(), &input);
    std::vector<std::complex<float>> output(size_t(doppler) * delays);
    for (unsigned d = 0; d < doppler; ++d) for (unsigned r = 0; r < delays; ++r)
      output[r * doppler + (d + doppler / 2 + 1) % doppler] = cpu.result()->data[d][r];
    failed->output = output; replacement->output = output;
    input.replace(std::deque<Complex>(raw));
    acceleration.process(reference.view_data(), {&input}, {&cpu}, [&] {
      ++cpuCalls; cpu.process(reference.view_data(), &input);
    });
  }
  check(creations == 2, "Transient worker recovery spawned more than one replacement");
  check(cpuCalls == 5, "Recovered worker skipped one-frame numerical requalification or retained CPU fallback");
  check(acceleration.status().active == "vulkan" && acceleration.status().state == "ready",
    "Recovered worker did not return to qualified GPU execution");
  check(acceleration.ambiguityGpuExecuted() && !acceleration.ambiguityCpuExecuted(),
    "Recovered steady frame did not execute exclusively on GPU");
  std::cout << "PASS transient worker self-recovery and numerical requalification\n";
}

int main(int argc, char**) {
  try {
    testPackedFrames();
    testClutterHostWork();
    testIndependentPortableLanesCanOverlap();
    testStartupPreparation();
    testTransientWorkerRecovery();
    for (const std::string mode : {"cpu", "auto", "gpu"}) {
      bool rejected = false;
      try {
        blah2::Acceleration invalid(mode, {400, 2401, 256, 1, -10}, 199, 2400000, 0);
      } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("Delay limits exceed the correlation block") != std::string::npos;
      }
      check(rejected, "Acceleration accepted a signed delay outside its correlation block");
    }
    for (const std::string fault : {"throw", "wrong", "nan", "nan-tail", "wrong-tail", "shape", "later", "init", "cpu", "missing"}) {
      if (fault == "missing" && argc == 1) continue; // Explicit no-driver run only.
      constexpr unsigned samples = 4800;
      Ambiguity cpu(-5, 20, -100, 100, 48000, samples, true);
      IqData input(samples);
      std::deque<Complex> reference(samples), raw(samples);
      for (unsigned i = 0; i < samples; ++i) {
        reference[i] = {std::sin(i * .37), std::cos(i * .61)};
        raw[i] = {std::sin(i * .41), std::cos(i * .17)};
      }
      input.replace(std::deque<Complex>(raw)); cpu.process(reference, &input);
      const auto expected = cpu.result()->data;
      auto state = std::make_shared<State>(); state->fault = fault;
      const unsigned doppler = cpu.get_n_doppler_bins(), delays = cpu.get_n_delay_bins();
      state->output.resize(doppler * delays);
      for (unsigned d = 0; d < doppler; ++d)
        for (unsigned r = 0; r < delays; ++r)
          state->output[r * doppler + (d + doppler / 2 + 1) % doppler] = expected[d][r];
      auto factory = [state](const blah2::GpuGeometry&, const std::string&) -> std::unique_ptr<blah2::GpuBackend> {
        if (state->fault == "init") throw std::runtime_error("No GPU driver");
        if (state->fault == "cpu") throw std::runtime_error("CPU mode called GPU factory");
        return std::make_unique<Backend>(state);
      };
      const std::string mode = fault == "cpu" ? "cpu" : "gpu";
      blah2::Acceleration acceleration(mode, {cpu.get_nfft(), doppler, delays, 1, -5},
        cpu.get_n_corr(), 48000, 0, "auto", fault == "missing" ? blah2::Acceleration::BackendFactory{} : factory);
      if (fault != "init" && fault != "cpu" && fault != "missing")
        check(acceleration.status().device == "test", "Portable telemetry lost exact device ID");
      unsigned calls = 0;
      for (unsigned frame = 0; frame < 5; ++frame) {
        input.replace(std::deque<Complex>(raw));
        acceleration.process(reference, {&input}, {&cpu}, [&] { ++calls; cpu.process(reference, &input); });
        check(cpu.result()->data == expected, "Fault did not preserve the CPU result");
        check(input.get_length() == samples - cpu.get_n_corr() * doppler, "Frame consumed twice");
      }
      check(calls == 5, "Expected one CPU execution per recovered frame");
      check(acceleration.status().active == "cpu", "Fault was not recovered on CPU");
      check(fault == "cpu" || acceleration.status().state == "fallback", "Fallback reason is absent");
      check(acceleration.retainedScratchBytes() == 0,
        "Retired GPU path retained host staging capacity");
      if (fault != "init" && fault != "cpu" && fault != "missing")
        check(state->calls <= (fault == "later" ? 4u : 1u), "Failed GPU was retried");
      std::cout << "PASS fallback=" << fault << '\n';
    }
    bool rejected = false;
    try { blah2::Acceleration invalid("cuda", {1,1,1,1,0}, 1, 1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Unknown acceleration setting accepted");

    // Deterministic clock: correct GPU=20 ms, CPU=1 ms at startup. AUTO must
    // retain the qualified GPU, preserving both timing measurements as facts.
    constexpr unsigned samples = 4800;
    Ambiguity cpu(-5, 20, -100, 100, 48000, samples, true);
    IqData input(samples);
    std::deque<Complex> reference(samples), raw(samples);
    for (unsigned i = 0; i < samples; ++i) {
      reference[i] = {std::sin(i * .37), std::cos(i * .61)};
      raw[i] = {std::sin(i * .41), std::cos(i * .17)};
    }
    input.replace(std::deque<Complex>(raw)); cpu.process(reference, &input);
    const auto expected = cpu.result()->data;
    auto gpuExpected = expected;
    for (auto& row : gpuExpected)
      for (auto& value : row) value = std::complex<double>(std::complex<float>(value));
    auto state = std::make_shared<State>();
    const unsigned doppler = cpu.get_n_doppler_bins(), delays = cpu.get_n_delay_bins();
    state->output.resize(doppler * delays);
    for (unsigned d = 0; d < doppler; ++d)
      for (unsigned r = 0; r < delays; ++r)
        state->output[r * doppler + (d + doppler / 2 + 1) % doppler] = expected[d][r];
    std::vector<double> ticks{0,20,21,22, 100,120,121,122, 200,220,221,222};
    for (unsigned frame = 0; frame < 10; ++frame) {
      ticks.push_back(300 + frame * 100);
      ticks.push_back(320 + frame * 100);
    }
    size_t tick = 0;
    auto clock = [&] { return ticks.at(tick++); };
    auto factory = [state](const blah2::GpuGeometry&, const std::string&)
      -> std::unique_ptr<blah2::GpuBackend> {
      return std::make_unique<Backend>(state);
    };
    blah2::Acceleration sustained("auto", {cpu.get_nfft(), doppler, delays, 1, -5},
      cpu.get_n_corr(), 48000, 0, "auto", factory, clock);
    unsigned cpuCalls = 0;
    for (unsigned frame = 0; frame < 13; ++frame) {
      input.replace(std::deque<Complex>(raw));
      sustained.process(reference, {&input}, {&cpu}, [&] { ++cpuCalls; cpu.process(reference, &input); });
      check(cpu.result()->data == (frame < 3 ? expected : gpuExpected),
        "Accepted GPU path changed the valid result");
      check(input.get_length() == samples - cpu.get_n_corr() * doppler,
        "Accepted GPU path consumed input twice");
    }
    check(cpuCalls == 3 && state->calls == 13, "Accepted GPU path reran a frame on CPU");
    check(sustained.status().active == "vulkan" && sustained.status().state == "ready" &&
      sustained.status().reason.empty(),
      "Whole-call latency overrode the like-for-like startup qualification");
    check(tick == ticks.size(), "Accepted GPU timing fixture was not fully consumed");
    check(sustained.status().gpuMs > sustained.status().cpuMs,
      "Slower-but-correct GPU timing evidence was not preserved");

    // Several successful windows stay bounded and do not create a cumulative
    // speed decision after the qualifying CPU measurements.
    auto fastState = std::make_shared<State>();
    fastState->output = state->output;
    auto fastFactory = [fastState](const blah2::GpuGeometry&, const std::string&)
      -> std::unique_ptr<blah2::GpuBackend> { return std::make_unique<Backend>(fastState); };
    std::vector<double> qualificationTicks{0,1,2,12, 100,101,102,112, 200,201,202,212};
    size_t qualificationTick = 0;
    double fastTick = 300;
    auto fastClock = [&]() {
      if (qualificationTick < qualificationTicks.size()) return qualificationTicks[qualificationTick++];
      return fastTick += 1;
    };
    blah2::Acceleration fast("auto", {cpu.get_nfft(), doppler, delays, 1, -5},
      cpu.get_n_corr(), 48000, 0, "auto", fastFactory, fastClock);
    unsigned fastCpuCalls = 0;
    for (unsigned frame = 0; frame < 23; ++frame) {
      input.replace(std::deque<Complex>(raw));
      fast.process(reference, {&input}, {&cpu}, [&] { ++fastCpuCalls; cpu.process(reference, &input); });
      check(cpu.result()->data == (frame < 3 ? expected : gpuExpected),
        "Fast AUTO changed the valid result");
      check(input.get_length() == samples - cpu.get_n_corr() * doppler,
        "Fast AUTO frame consumed input twice");
    }
    check(fastCpuCalls == 3 && fastState->calls == 23 &&
      fast.status().active == "vulkan" && fast.status().state == "ready",
      "Fast AUTO did not retain GPU across bounded sustained windows");

    // Forced GPU deliberately has no sustained-speed policy.
    auto forcedState = std::make_shared<State>();
    forcedState->output = state->output;
    auto forcedFactory = [forcedState](const blah2::GpuGeometry& geometry, const std::string&)
      -> std::unique_ptr<blah2::GpuBackend> {
      return std::make_unique<FrameBackend>(forcedState, geometry);
    };
    auto forcedClock = [value = 0.0]() mutable { return value += 1.0; };
    blah2::Acceleration forced("gpu", {cpu.get_nfft(), doppler, delays, 1, -5},
      cpu.get_n_corr(), 48000, 0, "auto", forcedFactory, forcedClock);
    unsigned forcedCpuCalls = 0;
    for (unsigned frame = 0; frame < 13; ++frame) {
      input.replace(std::deque<Complex>(raw));
      forced.process(reference, {&input}, {&cpu}, [&] { ++forcedCpuCalls; cpu.process(reference, &input); });
      check(cpu.result()->data == (frame < 3 ? expected : gpuExpected),
        "Forced GPU changed the valid result");
      check(input.get_length() == samples - cpu.get_n_corr() * doppler,
        "Forced GPU consumed input twice");
    }
    check(forcedCpuCalls == 3 && forcedState->calls == 13 &&
      forced.status().active == "vulkan" && forced.status().state == "ready",
      "Forced GPU was changed by the sustained AUTO guard");

    // An ambiguity-only numerical rejection must not discard a healthy clutter
    // accelerator in the same worker. Each stage owns its correctness
    // fallback; only a worker exception disables both.
    Ambiguity independentCpu(-5,20,-100,100,48000,samples,true);
    const blah2::GpuGeometry combinedGeometry{independentCpu.get_nfft(),
      independentCpu.get_n_doppler_bins(),independentCpu.get_n_delay_bins(),1,-5,
      samples,1,0};

    // Full clutter/FIR + composed ambiguity qualification remains selected
    // even when the deterministic GPU measurement exceeds the CPU oracle.
    auto hybridState=std::make_shared<State>();
    hybridState->output=state->output;
    auto hybridFactory=[hybridState](const blah2::GpuGeometry& geometry,const std::string&)
      ->std::unique_ptr<blah2::GpuBackend>{return std::make_unique<CombinedBackend>(geometry,hybridState);};
    constexpr unsigned hybridSteadyFrames=12;
    double hybridTick=0;
    auto hybridClock=[&]{return ++hybridTick;};
    blah2::Acceleration hybrid("auto",combinedGeometry,independentCpu.get_n_corr(),
      48000,0,"auto",hybridFactory,hybridClock);
    IqData hybridReference(samples),hybridInput(samples);
    unsigned hybridClutterCpu=0,hybridAmbiguityCpu=0;
    const auto runHybrid=[&]{
      hybridReference.replace(std::deque<Complex>(reference));
      hybridInput.replace(std::deque<Complex>(raw));
      check(hybrid.processClutter(hybridReference,{&hybridInput},[&]{
        ++hybridClutterCpu; return true;
      }),"Hybrid clutter frame failed");
      hybrid.process(reference,{&hybridInput},{&independentCpu},[&]{
        ++hybridAmbiguityCpu; independentCpu.process(reference,&hybridInput);
      });
    };
    constexpr unsigned hybridQualification=blah2::Acceleration::TotalQualificationFrames;
    for(unsigned frame=0;frame<hybridQualification;++frame) runHybrid();
    check(hybrid.clutterStatus().active=="vulkan" &&
      hybrid.clutterStatus().state=="ready" &&
      hybrid.clutterStatus().gpuMs>hybrid.clutterStatus().cpuMs &&
      hybrid.status().active=="vulkan" && hybrid.status().state=="ready",
      "AUTO rejected slower-but-correct GPU clutter or ambiguity");
    for(unsigned frame=0;frame<hybridSteadyFrames;++frame) runHybrid();
    check(hybridClutterCpu==hybridQualification && hybridAmbiguityCpu==hybridQualification &&
      hybridState->calls==hybridQualification+hybridSteadyFrames &&
      hybridState->clutterCalls==hybridQualification+hybridSteadyFrames &&
      hybrid.status().active=="vulkan" && hybrid.clutterStatus().active=="vulkan",
      "Slow qualified GPU stages reran steady work on CPU");

    // A portable worker may fail in ambiguity after its clutter operation.
    // The next CPI initially selects stages from the fallback state, so worker
    // recreation must be polled at that boundary rather than only from the
    // now-unselected GPU clutter method.
    hybridState->fault="ambiguity-transient";
    runHybrid();
    check(!hybrid.available() && hybrid.status().active=="cpu" &&
      hybrid.clutterStatus().active=="cpu",
      "Transient combined-worker failure did not enter same-frame CPU fallback");
    hybridState->fault.clear();
    hybrid.beginFrame();
    check(hybrid.available() && hybrid.clutterAvailable() &&
      hybrid.status().state=="checking" && hybrid.clutterStatus().state=="checking",
      "CPI-boundary recovery did not restore the combined GPU worker for requalification");
    runHybrid();
    check(hybrid.status().active=="vulkan" && hybrid.clutterStatus().active=="vulkan" &&
      hybrid.ambiguityCpuExecuted() && hybrid.clutterTiming().cpuExecuted,
      "Recovered combined worker skipped bounded CPU numerical requalification");

    auto combinedState=std::make_shared<State>();
    combinedState->output=state->output; combinedState->fault="wrong";
    auto combinedFactory=[combinedState](const blah2::GpuGeometry& geometry,const std::string&)
      ->std::unique_ptr<blah2::GpuBackend>{return std::make_unique<CombinedBackend>(geometry,combinedState);};
    blah2::Acceleration independent("gpu",combinedGeometry,independentCpu.get_n_corr(),
      48000,0,"auto",combinedFactory);
    IqData independentReference(samples),independentInput(samples);
    unsigned independentCpuCalls=0, clutterCpuCalls=0;
    for(unsigned frame=0;frame<blah2::Acceleration::TotalQualificationFrames;++frame){
      independentReference.replace(std::deque<Complex>(reference));
      independentInput.replace(std::deque<Complex>(raw));
      check(independent.processClutter(independentReference,{&independentInput},[&]{
        ++clutterCpuCalls; return true;
      }),
        "Combined clutter qualification failed");
      independent.process(reference,{&independentInput},{&independentCpu},[&]{
        ++independentCpuCalls; independentCpu.process(reference,&independentInput);
      });
    }
    check(independent.status().active=="cpu" && independent.clutterStatus().active=="vulkan",
      "CPU ambiguity did not retain independently qualified GPU clutter");
    const unsigned beforeClutterCpu=clutterCpuCalls;
    const unsigned beforeAmbiguityCpu=independentCpuCalls;
    constexpr unsigned steadyFrames=64; // Cross several former 16-frame oracle boundaries.
    check(beforeClutterCpu==blah2::Acceleration::TotalQualificationFrames &&
      beforeAmbiguityCpu==blah2::Acceleration::TotalQualificationFrames+
        blah2::Acceleration::CombinedQualificationFrames,
      "CPU ambiguity/GPU clutter startup qualification count changed");
    for(unsigned frame=0;frame<steadyFrames;++frame){
      independentReference.replace(std::deque<Complex>(reference));
      independentInput.replace(std::deque<Complex>(raw));
      check(independent.processClutter(independentReference,{&independentInput},[&]{
        ++clutterCpuCalls; return true;
      }),"Healthy clutter GPU failed after ambiguity fallback");
      independent.process(reference,{&independentInput},{&independentCpu},[&]{
        ++independentCpuCalls; independentCpu.process(reference,&independentInput);
      });
      check(clutterCpuCalls==beforeClutterCpu && independent.clutterTiming().gpuExecuted &&
        !independent.clutterTiming().cpuExecuted,
        "Steady GPU clutter ran a CPU oracle with CPU ambiguity selected");
      check(independentCpuCalls==beforeAmbiguityCpu+frame+1,
        "Steady CPU ambiguity recomputed a composed oracle");
      check(independentCpu.result()->data==expected &&
        independentInput.get_length()==samples-independentCpu.get_n_corr()*doppler,
        "CPU ambiguity/GPU clutter changed the result or consumed input twice");
    }

    // Every new instance still qualifies all eight startup frames. Afterwards
    // neither accepted GPU stage may invoke a CPU oracle, in AUTO or GPU mode.
    // Faults are injected only after qualification, except the deliberately
    // mismatched composed-startup case below.
    for(const std::string mode:{"auto","gpu"})
      for(const std::string fault:{"", "wrong", "clutter-throw", "clutter-nan",
          "clutter-reject", "ambiguity-throw", "ambiguity-nan", "ambiguity-shape"}){
        auto acceptedState=std::make_shared<State>();
        acceptedState->output=state->output;
        auto acceptedFactory=[acceptedState](const blah2::GpuGeometry& geometry,const std::string&)
          ->std::unique_ptr<blah2::GpuBackend>{return std::make_unique<CombinedBackend>(geometry,acceptedState);};
        double now=0;
        auto acceptedClock=[&]{return ++now;};
        blah2::Acceleration accepted(mode,combinedGeometry,independentCpu.get_n_corr(),
          48000,0,"auto",acceptedFactory,acceptedClock);
        unsigned acceptedClutterCpu=0, acceptedAmbiguityCpu=0;
        const auto runFrame=[&]{
          independentReference.replace(std::deque<Complex>(reference));
          independentInput.replace(std::deque<Complex>(raw));
          check(accepted.processClutter(independentReference,{&independentInput},[&]{
            ++acceptedClutterCpu; now+=100; return true;
          }),"Combined lifetime clutter frame failed");
          accepted.process(reference,{&independentInput},{&independentCpu},[&]{
            ++acceptedAmbiguityCpu; now+=100;
            independentCpu.process(reference,&independentInput);
          });
          check(independentInput.get_length()==samples-independentCpu.get_n_corr()*doppler,
            "Combined lifetime frame consumed input twice");
        };
        accepted.prepareStartup([&](unsigned frame){
          if(fault=="wrong" && frame==blah2::Acceleration::ClutterQualificationFrames)
            acceptedState->fault=fault;
          runFrame();
          check(acceptedClutterCpu==frame+1 && acceptedAmbiguityCpu==frame+1,
            "A startup frame skipped its independent CPU qualification");
          check(independentCpu.result()->data==expected,
            "CPU did not own the startup qualification result");
        });
        check(!accepted.startupPending(), "Combined startup returned before all checks settled");
        if(fault=="wrong"){
          check(accepted.clutterStatus().state=="fallback" &&
            accepted.clutterStatus().reason==
              "Composed GPU clutter/ambiguity accuracy check failed; using CPU clutter" &&
            accepted.status().active=="vulkan",
            "Startup composed mismatch did not reject only GPU clutter");
          continue;
        }
        check(accepted.clutterStatus().active=="vulkan" &&
          accepted.status().active=="vulkan", "Both GPU stages did not finish startup qualification");
        for(unsigned frame=0;frame<steadyFrames;++frame){
          runFrame();
          check(acceptedClutterCpu==blah2::Acceleration::TotalQualificationFrames &&
            acceptedAmbiguityCpu==blah2::Acceleration::TotalQualificationFrames &&
            accepted.clutterTiming().gpuExecuted && !accepted.clutterTiming().cpuExecuted,
            "Accepted steady GPU frame ran a CPU oracle");
          check(independentCpu.result()->data==gpuExpected,
            "Accepted steady GPU map changed");
        }
        check(acceptedState->calls==blah2::Acceleration::TotalQualificationFrames+steadyFrames &&
          acceptedState->clutterCalls==acceptedState->calls &&
          accepted.clutterStatus().active=="vulkan" && accepted.status().active=="vulkan",
          "Steady GPU stage was skipped or AUTO lost a healthy stage");
        if(!fault.empty()){
          acceptedState->fault=fault;
          runFrame();
          check(accepted.clutterStatus().state=="fallback",
            "Post-qualification GPU fault was not recovered");
          check(accepted.clutterStatus().gpuExecutionBackend ==
              (accepted.clutterTiming().gpuExecuted ? "vulkan" : ""),
            "Clutter CPU retry lost completed GPU identity");
          check(accepted.status().gpuExecutionBackend ==
              (accepted.ambiguityGpuExecuted() ? "vulkan" : ""),
            "Ambiguity CPU retry lost completed GPU identity");
          const bool clutterFault=fault.find("clutter-")==0;
          check(acceptedClutterCpu==blah2::Acceleration::TotalQualificationFrames+(clutterFault?1:0),
            "Post-qualification clutter CPU fallback call count changed");
          check(acceptedAmbiguityCpu==blah2::Acceleration::TotalQualificationFrames+
              (fault=="clutter-reject"?0:1),
            "Post-qualification ambiguity CPU fallback call count changed");
          check(independentCpu.result()->data==(fault=="clutter-reject"?gpuExpected:expected),
            "Post-qualification fault did not preserve the recovered map");
          const unsigned gpuCalls=acceptedState->calls, clutterCalls=acceptedState->clutterCalls;
          runFrame();
          check(accepted.clutterStatus().gpuExecutionBackend.empty(),
            "CPU-only next frame retained stale clutter GPU execution identity");
          check(accepted.status().gpuExecutionBackend ==
              (accepted.ambiguityGpuExecuted() ? "vulkan" : ""),
            "CPU-only next frame retained stale ambiguity GPU execution identity");
          check(acceptedState->clutterCalls==clutterCalls &&
            (fault=="clutter-reject"?acceptedState->calls==gpuCalls+1:acceptedState->calls==gpuCalls),
            "Failed GPU stage was retried after steady-state fallback");
        }
        std::cout<<"PASS startup-only mode="<<mode<<" steady_frames="<<steadyFrames
          <<" fault="<<(fault.empty()?"none":fault)<<'\n';
      }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
