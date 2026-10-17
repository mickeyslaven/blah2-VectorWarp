#include "GpuResidentBackend.h"
#include "GpuResidentProof.h"
#include "GpuResidentWire.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace blah2::resident;

namespace {
Config configuration(const char* device) {
  Config c;
  auto& g = c.geometry;
  g.kind = blah2::GpuWorkKind::radar;
  g.range = 12;
  g.doppler = 7;
  g.delays = 3;
  g.channels = 5;
  g.delayMin = -1;
  g.clutterSamples = 64;
  g.clutterBins = 4;
  c.correlation = 9;
  c.sampleRate = 128000;
  c.epoch = 19;
  c.surveillanceChannels = {0, 1, 2, 3, 4};
  c.delayAxis = {-1, 0, 1};
  const double resolution = 1.0 / (63.0 / c.sampleRate);
  c.dopplerAxis = {-3 * resolution, -2 * resolution, -resolution, 0,
    resolution, 2 * resolution, 3 * resolution};
  c.spectrumBins = 64;
  c.reference.channels = {0, 1, 2, 3, 4};
  c.reference.analysisSamples = 64;
  c.fusion = {FusionMode::coherentRegularPolygon, 12, 539000000,
    .193548, 0, -2 * std::acos(-1.0) / 5, 80 * std::acos(-1.0) / 180, 6};
  c.detector.enabled = true;
  c.detector.guard = 1;
  c.detector.train = 2;
  c.detector.pfa = .001;
  c.detector.centroidDopplerResolution = resolution;
  c.detector.integrationCpis = 8;
  c.detector.integrationMotionBins = 1;
  c.device = device;
  c.presence = wire::mandatoryPresence(c);
  return c;
}
}

int main(int argc, char** argv) try {
  if (argc != 3) throw std::invalid_argument(
    "usage: testGpuResidentTbdPhysical WORKER DEVICE_ID");
  blah2::GpuProcessOptions options;
  options.executable = argv[1];
  options.startupMs = 60000;
  options.frameMs = 10000;
  auto session = createProcess(configuration(argv[2]), options);
  for (uint64_t frame = 1; frame <= 9; ++frame) {
    auto input = session->input();
    for (size_t i = 0; i < input.size; ++i) {
      const double phase = .017 * double(i) + .031 * double(frame) +
        .11 * double(i % 5);
      input.data[i] = {std::cos(phase), std::sin(phase)};
    }
    session->begin({frame, frame, 1000 + frame});
    session->receiveReferenceReady();
    const auto output = session->receivePrepared();
    const auto proof = preparedProof(output.preparedEvidence);
    const bool ready = proof.postprocessStatus[28] != 0;
    if (ready != (frame >= 8))
      throw std::runtime_error("TBD history readiness changed");
    session->commit(output.trackerToken);
    session->published();
  }
  std::cout << "GPU_RESIDENT_TBD_PHYSICAL_PASS frames=9 integration_cpis=8"
    " channels=5 beams=12 history_ready_frame=8\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL resident TBD physical initialization: " << error.what() << '\n';
  return 1;
}
