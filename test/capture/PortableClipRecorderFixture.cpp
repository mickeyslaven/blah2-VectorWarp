#include "capture/PortableClipRecorder.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <thread>
#include <unistd.h>

namespace {
blah2::IqBlock block(unsigned first, unsigned count) {
  blah2::IqBlock result(2, std::vector<std::complex<float>>(count));
  for (unsigned channel = 0; channel < 2; ++channel)
    for (unsigned i = 0; i < count; ++i)
      result[channel][i] = {float(first + i), float(channel)};
  return result;
}

blah2::IqClipStatus wait(blah2::PortableClipRecorder& recorder) {
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    const auto status = recorder.status();
    if (status.terminal) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(false && "portable clip writer did not finish");
  return {};
}
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
    ("vectorwarp-portable-clips-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  try {
    blah2::PortableClipRecorder::Options options;
    options.ringSeconds = 0.3;
    options.maximumClipSeconds = 0.5;
    blah2::PortableClipRecorder recorder(directory.string(), "Usrp", 2, 1000,
      204640000, options);

    // Receipt time describes the end of each block, establishing a zero-time
    // sample epoch while retaining only the configured 300-sample pre-roll.
    for (unsigned first = 0; first < 300; first += 100)
      assert(recorder.ingest(block(first, 100), 7, first, true,
        int64_t(first + 100) * 1000000));
    recorder.request({1, 300, 150, 450, {"event"}, {"pass"}});
    assert(recorder.ingest(block(300, 100), 7, 300, true, 400000000));
    assert(recorder.ingest(block(400, 100), 7, 400, true, 500000000));
    const auto status = wait(recorder);
    assert(status.revision == 1 && status.error.empty());
    assert(status.samples == 300 && !status.file.empty());
    std::ifstream metadataInput(status.file + ".json");
    const std::string metadataText(std::istreambuf_iterator<char>(metadataInput), {});
    rapidjson::Document metadata; metadata.Parse(metadataText.c_str());
    assert(!metadata.HasParseError() && metadata.IsObject());
    assert(std::string(metadata["schema"].GetString()) ==
      "vectorwarp-portable-iq-clip-v1");
    assert(metadata["clipRevision"].GetUint64() == 1);
    assert(metadata["epoch"].GetUint64() == 7);
    assert(metadata["firstEpochSample"].GetUint64() == 150);
    assert(metadata["endEpochSample"].GetUint64() == 450);
    assert(metadata["samplesPerChannel"].GetUint64() == 300);
    assert(metadata["dataSha256"].IsString() &&
      metadata["dataSha256"].GetStringLength() == 64);
    assert(metadata["dataBytes"].GetUint64() == std::filesystem::file_size(status.file));
    assert(!std::filesystem::exists(status.file + ".partial"));
    assert(!std::filesystem::exists(status.file + ".json.partial"));

    blah2::ReplayOptions replay;
    replay.format = "auto"; replay.receiver = "Usrp"; replay.channels = 2;
    replay.sampleRate = 1000; replay.frequency = 204640000;
    blah2::RecordingReader reader(status.file, replay);
    assert(reader.metadata().samples == 300);
    blah2::IqBlock samples, part;
    while (reader.read(part)) {
      if (samples.empty()) samples.resize(part.size());
      for (unsigned channel = 0; channel < part.size(); ++channel)
        samples[channel].insert(samples[channel].end(), part[channel].begin(),
          part[channel].end());
    }
    assert(samples.size() == 2 && samples[0].size() == 300);
    assert(std::abs(samples[0].front().real() - 150.0f) < 0.001f);
    assert(std::abs(samples[1].back().real() - 449.0f) < 0.001f);

    // The bounded ring rejects pre-roll that has already aged out.
    recorder.request({2, 500, 0, 100, {}, {}});
    const auto old = recorder.status();
    assert(old.terminal && old.revision == 2 && !old.error.empty());

    // An in-flight post-roll never crosses a receiver discontinuity.
    recorder.request({3, 500, 450, 650, {}, {}});
    recorder.discontinuity("test sample gap");
    const auto gap = recorder.status();
    assert(gap.terminal && gap.revision == 3 && gap.error == "test sample gap");

    std::filesystem::remove_all(directory);
    std::cout << "portable bounded clip recorder checks passed\n";
    return 0;
  } catch (...) {
    std::filesystem::remove_all(directory);
    throw;
  }
}
