#include "process/ambiguity/Acceleration.h"
#include <chrono>
#include <cmath>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using Complex = std::complex<double>;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct State { std::string fault; unsigned calls = 0; std::vector<std::complex<float>> output; };
struct Backend : blah2::GpuBackend {
  std::shared_ptr<State> state;
  explicit Backend(std::shared_ptr<State> value) : state(std::move(value)) {}
  blah2::GpuDevice device() const override { return {"test", "Test double", 0}; }
  void process(const std::vector<std::complex<float>>&, const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>& output) override {
    ++state->calls;
    if (state->fault == "throw" || (state->fault == "later" && state->calls > 3))
      throw std::runtime_error("GPU device lost; using CPU");
    if (state->fault == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    return {reference.data(), reference.size(), surveillance.data(), surveillance.size(),
      output.data(), output.size()};
  }
  void processFrame() override { ++state->calls; output = state->output; }
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
    ++state->calls; output=state->output;
    if(state->fault=="wrong") for(auto& value:output) value*=2;
  }
  bool clutterAvailable() const override { return true; }
  blah2::GpuClutterBuffers clutterBuffers() override {
    return {clutterReference.data(),clutterReference.size(),
      clutterSurveillance.data(),clutterSurveillance.size(),
      clutterOutput.data(),clutterOutput.size()};
  }
  bool processClutterFrame() override {
    std::fill(clutterOutput.begin(),clutterOutput.end(),std::complex<float>{});
    return true;
  }
};
int main(int argc, char**) {
  try {
    for (const std::string mode : {"cpu", "auto", "gpu"}) {
      bool rejected = false;
      try {
        blah2::Acceleration invalid(mode, {400, 2401, 256, 1, -10}, 199, 2400000, 0);
      } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("Delay limits exceed the correlation block") != std::string::npos;
      }
      check(rejected, "Acceleration accepted a signed delay outside its correlation block");
    }
    for (const std::string fault : {"throw", "wrong", "nan", "shape", "later", "slow", "init", "cpu", "missing"}) {
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
      const std::string mode = fault == "cpu" ? "cpu" : fault == "slow" ? "auto" : "gpu";
      blah2::Acceleration acceleration(mode, {cpu.get_nfft(), doppler, delays, 1, -5},
        cpu.get_n_corr(), 48000, 0, "auto", fault == "missing" ? blah2::Acceleration::BackendFactory{} : factory);
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
        check(state->calls <= (fault == "later" ? 4u : fault == "slow" ? 3u : 1u), "Failed GPU was retried");
      std::cout << "PASS fallback=" << fault << '\n';
    }
    bool rejected = false;
    try { blah2::Acceleration invalid("cuda", {1,1,1,1,0}, 1, 1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Unknown acceleration setting accepted");

    // Deterministic clock: qualification sees GPU=1 ms, CPU=10 ms. Five fast
    // frames pass their window; the following five slow frames make AUTO
    // select CPU only for future frames without rerunning one.
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
    std::vector<double> ticks{0,1,2,12, 100,101,102,112, 200,201,202,212,
      300,301,305, 400,401,405, 500,501,505, 600,601,605, 700,701,705,
      800,801,820, 900,901,920, 1000,1001,1020, 1100,1101,1120, 1200,1201,1220};
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
        "Sustained GPU guard changed the valid result");
      check(input.get_length() == samples - cpu.get_n_corr() * doppler,
        "Sustained GPU guard consumed input twice");
    }
    check(cpuCalls == 3 && state->calls == 13, "Sustained GPU guard reran an accepted frame");
    check(sustained.status().active == "cpu" && sustained.status().state == "fallback" &&
      sustained.status().reason == "CPU was faster than sustained GPU processing",
      "Sustained GPU guard did not choose CPU for future frames");

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
    // accelerator in the same worker. Each stage owns its performance/correctness
    // fallback; only a worker exception disables both.
    Ambiguity independentCpu(-5,20,-100,100,48000,samples,true);
    const blah2::GpuGeometry combinedGeometry{independentCpu.get_nfft(),
      independentCpu.get_n_doppler_bins(),independentCpu.get_n_delay_bins(),1,-5,
      samples,1,0};
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
    independentReference.replace(std::deque<Complex>(reference));
    independentInput.replace(std::deque<Complex>(raw));
    const unsigned beforeClutterCpu=clutterCpuCalls;
    check(independent.processClutter(independentReference,{&independentInput},[&]{
      ++clutterCpuCalls; return true;
    }),"Healthy clutter GPU failed after ambiguity fallback");
    independent.process(reference,{&independentInput},{&independentCpu},[&]{
      ++independentCpuCalls; independentCpu.process(reference,&independentInput);
    });
    check(clutterCpuCalls==beforeClutterCpu && independent.clutterTiming().gpuExecuted,
      "GPU clutter was not retained with CPU ambiguity");
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
