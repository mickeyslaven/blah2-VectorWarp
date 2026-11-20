#include "capture/PortableClipRecorder.h"
#include "capture/RecordingAcquisitionJson.h"

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

bool ingest_when_available(blah2::PortableClipRecorder& recorder,
    unsigned first, unsigned count, uint64_t epoch, int64_t receiptNanoseconds,
    std::shared_ptr<const blah2::RecordingBlockMetadata> metadata = {}) {
  // Ingest waits only for the recorder's short state transition. File writes
  // stay outside this mutex, so a good receiver block is never discarded.
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    if (recorder.ingest(block(first, count), epoch, first, true,
        receiptNanoseconds, metadata)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
    ("vectorwarp-portable-clips-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  try {
    // Ten-second late claims are available only when the bounded float-IQ ring
    // fits the fixed admission budget; never let a high-rate portable radio
    // silently pin multiple GiB of RAM.
    assert(blah2::PortableClipRecorder::supports_retention(2, 2000000, 10.0));
    assert(!blah2::PortableClipRecorder::supports_retention(2, 20000000, 10.0));
    assert(!blah2::PortableClipRecorder::supports_retention(2, 30000000, 10.0));
    blah2::PortableClipRecorder::Options rejectedOptions;
    rejectedOptions.ringSeconds = 10.0; rejectedOptions.maximumClipSeconds = 10.0;
    bool rejected = false;
    try { blah2::PortableClipRecorder rejectedRecorder(directory.string(), "HackRF", 2,
      20000000, 204640000, rejectedOptions); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    blah2::PortableClipRecorder::Options options;
    options.ringSeconds = 0.3;
    options.maximumClipSeconds = 0.5;
    blah2::PortableClipRecorder recorder(directory.string(), "Usrp", 2, 1000,
      204640000, options);
    auto acquisition = std::make_shared<blah2::RecordingBlockMetadata>();
    acquisition->perChannelGainDb = {14.199999809265137, 0.10000000000000002};
    acquisition->gainProvenance = "sample-aligned-readback";
    acquisition->receiverDetailsJson = "{\"device\":\"B210-fixture\",\"readbackAvailable\":true}";
    acquisition->context.referenceMode = "dedicated";
    acquisition->context.referenceChannels = {0}; acquisition->context.surveillanceChannels = {1};
    acquisition->context.coherent = true;
    acquisition->context.configurationSha256 = std::string(64, 'a');
    acquisition->context.codeSha256 = std::string(64, 'b');
    acquisition->context.manifoldSha256 = {{"reference", std::string(64, 'c')}};

    // Receipt time describes the end of each block, establishing a zero-time
    // sample epoch while retaining only the configured 300-sample pre-roll.
    for (unsigned first = 0; first < 300; first += 100)
      assert(ingest_when_available(recorder, first, 100, 7,
        int64_t(first + 100) * 1000000, acquisition));
    recorder.request({1, 300, 150, 450, {"event"}, {"pass"}});
    assert(ingest_when_available(recorder, 300, 100, 7, 400000000, acquisition));
    assert(ingest_when_available(recorder, 400, 100, 7, 500000000, acquisition));
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
    assert(reader.provenance().perChannelGainDb == acquisition->perChannelGainDb);
    assert(reader.provenance().exactAcquisition && reader.provenance().acquisitionIntegrityValid);
    assert(!reader.provenance().acquisitionHashTrusted && !reader.provenance().exactTime);
    assert(reader.provenance().frames.size() == 4);
    assert(reader.provenance().frames.front().sourceFirst == 100);
    assert(reader.provenance().frames.front().sliceOffset == 50);
    assert(reader.provenance().frames.back().count == 50);
    assert(reader.provenance().acquisitionContext.manifoldSha256 == acquisition->context.manifoldSha256);
    auto strict = replay; strict.requireExactAcquisition = true;
    bool missingTrustRejected = false;
    try { blah2::RecordingReader invalid(status.file, strict); }
    catch (const std::exception&) { missingTrustRejected = true; }
    assert(missingTrustRejected);
    strict.expectedRecordingSha256 = metadata["dataSha256"].GetString();
    blah2::RecordingReader verified(status.file, strict);
    assert(verified.provenance().acquisitionHashTrusted);
    strict.expectedRecordingSha256[0] = strict.expectedRecordingSha256[0] == 'a' ? 'b' : 'a';
    bool tamperedTrustRejected = false;
    try { blah2::RecordingReader invalid(status.file, strict); }
    catch (const std::exception&) { tamperedTrustRejected = true; }
    assert(tamperedTrustRejected);
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
