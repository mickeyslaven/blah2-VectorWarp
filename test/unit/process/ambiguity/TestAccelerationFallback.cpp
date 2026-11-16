#include "process/ambiguity/Acceleration.h"
#include "process/analysis/StartupIq.h"
#include <cmath>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <algorithm>

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
    output = state->output;
    if (state->fault == "wrong") for (auto& item : output) item *= 2;
    if (state->fault == "nan") output[0] = {std::numeric_limits<float>::quiet_NaN(), 0};
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

int main(int argc, char**) {
  try {
    testPackedFrames();
    testStartupPreparation();
    for (const std::string mode : {"cpu", "auto", "gpu"}) {
      bool rejected = false;
      try {
        blah2::Acceleration invalid(mode, {400, 2401, 256, 1, -10}, 199, 2400000, 0);
      } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("Delay limits exceed the correlation block") != std::string::npos;
      }
      check(rejected, "Acceleration accepted a signed delay outside its correlation block");
    }
    for (const std::string fault : {"throw", "wrong", "nan", "shape", "later", "init", "cpu", "missing"}) {
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
