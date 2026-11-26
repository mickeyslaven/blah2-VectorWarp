// JSONL offline aperture export. Only a final complete record seals a run.
#include "process/integration/PassiveIsarAperture.h"
#include "WienerHopfReplayContract.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/utility/FftwThreads.h"
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cr = blah2::integration::coherent_replay;
using Writer = rapidjson::Writer<rapidjson::OStreamWrapper>;
namespace {
template<class Container> std::string hex(const Container& bytes) {
  std::string result; for (auto b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; } return result;
}
void text(Writer& w, const char* key, const std::string& value) { w.Key(key); w.String(value.c_str()); }
void integer(Writer& w, const char* key, uint64_t value) { text(w, key, std::to_string(value)); }
void number(Writer& w, const char* key, double value) { w.Key(key); w.Double(value); }
void complex(Writer& w, std::complex<double> z) { w.StartArray(); w.Double(z.real()); w.Double(z.imag()); w.EndArray(); }
template<class Container> void complexArray(Writer& w, const char* key, const Container& values) {
  w.Key(key); w.StartArray(); for (auto z : values) complex(w, z); w.EndArray();
}
std::vector<double> list(const char* value) {
  std::vector<double> result; std::stringstream input(value); std::string item;
  while (std::getline(input, item, ',')) { size_t used = 0;
    const double parsed = std::stod(item, &used);
    if (used != item.size() || !std::isfinite(parsed) || result.size() == 129) throw std::invalid_argument("Invalid finite bounded list");
    result.push_back(parsed); }
  if (result.empty() || std::string(value).back() == ',') throw std::invalid_argument("Empty list");
  return result;
}
uint32_t id(double value) {
  if (value < 0 || value > UINT32_MAX || value != std::floor(value)) throw std::invalid_argument("Invalid channel ID");
  return uint32_t(value);
}
blah2::resident_replay::Hash hashArgument(const char* value) {
  const std::string text(value); blah2::resident_replay::Hash result{};
  if (text.size() != 64) throw std::invalid_argument("Expected a lowercase SHA-256 pin");
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) throw std::invalid_argument("Invalid SHA-256 pin");
    result[i / 2] = uint8_t(result[i / 2] * 16 + (c <= '9' ? c - '0' : c - 'a' + 10));
  }
  return result;
}
cr::ExperimentReader::ApertureInput product(cr::AuthenticatedIqSegment segment) {
  const uint64_t span = uint64_t(int64_t(segment.dopplerMax) - segment.dopplerMin);
  const uint64_t upperRows = span * segment.raw.size() / segment.sampleRate + 3;
  const uint64_t columns = uint64_t(int64_t(segment.delayMax) - segment.delayMin + 1);
  if (upperRows > 511 || upperRows * columns > 65536) throw std::invalid_argument("Derived CAF exceeds aperture admission bound");
  Ambiguity ambiguity(segment.delayMin, segment.delayMax, segment.dopplerMin, segment.dopplerMax,
    segment.sampleRate, uint32_t(segment.raw.size()), true);
  IqData surveillance(uint32_t(segment.filtered.size()));
  for (auto z : segment.filtered) surveillance.push_back(z);
  std::deque<std::complex<double>> reference(segment.reference.begin(), segment.reference.end());
  cr::ExperimentReader::ApertureInput input;
  input.source = segment.parent; input.derived = std::move(segment.lineage);
  input.sequence = segment.sequence; input.firstSample = segment.firstSample; input.endSample = segment.endSample;
  input.correlationSamples = ambiguity.get_n_corr();
  // The authenticated plan records tuning, not an independently qualified
  // illumination carrier. Leave carrier unknown: only zero-rate diagnostics
  // are allowed until a separately sealed physical carrier contract exists.
  input.carrierHz = 0;
  const blah2::integration::DirectPathCoherentExperiment::Frame frame{segment.parent.captureEpoch,
    segment.sequence, segment.firstSample, segment.endSample, segment.parent.tuningHz,
    segment.parent.gainCalibrationEpoch, segment.sampleRate, true, true};
  input.pilot = blah2::integration::DirectPathCoherentExperiment::estimatePilot(segment.reference.data(),
    segment.raw.data(), segment.raw.size(), frame, segment.channelId);
  input.caf.push_back(*ambiguity.process(reference, &surveillance));
  return input;
}
std::string executableHash(const char* argument) {
  const char* env = std::getenv("PATH");
  const auto path = blah2::integration::cancellation_replay::runningExecutablePath(argument, env ? env : "");
  if (std::filesystem::file_size(path) > (128u << 20)) throw std::invalid_argument("Executable exceeds bound");
  std::ifstream file(path, std::ios::binary); blah2::Sha256 digest; std::array<char, 65536> buffer;
  while (file.read(buffer.data(), buffer.size()) || file.gcount()) digest.update(buffer.data(), size_t(file.gcount()));
  if (!file.eof()) throw std::runtime_error("Executable hash failed");
  return digest.finish();
}
void frameRecord(Writer& w, const cr::PassiveIsarAperture::Entry& e, const std::string& exe) {
  const auto& f = *e.frame; const auto& s = f.source;
  const auto correlation = f.correlationSamples, rows = f.migration[0].rows;
  w.StartObject(); text(w, "schema", "vectorwarp-passive-isar-aperture-v1"); text(w, "record", "frame");
  text(w, "executable_sha256", exe); text(w, "snapshot_sha256", hex(s.snapshotHash));
  text(w, "snapshot_and_sidecar_sha256", hex(blah2::integration::cancellation_replay::frameIdentity(s)));
  text(w, "source_sha256", hex(s.sourceHash)); text(w, "rf_sha256", hex(s.rfHash));
  text(w, "config_sha256", hex(s.identity.configHash)); text(w, "module_sha256", hex(s.identity.moduleHash));
  text(w, "device_sha256", hex(s.identity.deviceHash)); text(w, "raw_iq_sha256", hex(s.identity.rawIqHash));
  integer(w, "capture_epoch", s.captureEpoch); integer(w, "resident_epoch", s.identity.epoch);
  text(w, "origin", f.derived ? "authenticated_derived_iq_not_native_snapshot" : "sealed_native_snapshot");
  integer(w, "sequence", f.sequence); integer(w, "parent_sequence", s.identity.logicalFrame);
  integer(w, "generation", s.identity.generation);
  integer(w, "consumed_sequence", s.identity.consumedSequence); integer(w, "signal_timestamp_ms", s.identity.signalTimestampMs);
  integer(w, "first_sample", f.firstSample); integer(w, "end_sample", f.endSample);
  integer(w, "parent_first_sample", s.firstSample); integer(w, "parent_end_sample", s.endSample);
  integer(w, "new_raw_first", e.newRawFirst); integer(w, "new_raw_end", e.newRawEnd);
  integer(w, "processed_end", f.firstSample + uint64_t(rows) * correlation);
  integer(w, "unprocessed_tail_samples", e.unprocessedTailSamples);
  integer(w, "missing_support_before_samples", e.missingBeforeSamples);
  integer(w, "tuning_hz", s.tuningHz); integer(w, "gain_calibration_epoch", s.gainCalibrationEpoch);
  integer(w, "sample_rate_hz", s.sampleRateHz); integer(w, "correlation_samples", correlation);
  w.Key("carrier_hz"); if (f.carrierHz > 0) w.Double(f.carrierHz); else w.Null();
  w.Key("reference_conditioning_bandwidth_hz");
  if (std::isfinite(f.referenceConditioningBandwidthHz)) w.Double(f.referenceConditioningBandwidthHz); else w.Null();
  w.Key("effective_occupied_bandwidth_hz"); w.Null(); w.Key("external_manifold_qualified"); w.Bool(false);
  w.Key("source_config_wire_hex");
  if (f.sourceConfig) w.String(hex(blah2::resident::encodeConfig(*f.sourceConfig)).c_str()); else w.Null();
  if (f.derived) {
    text(w, "derived_plan_sha256", hex(f.derived->planHash)); text(w, "extraction_receipt_sha256", hex(f.derived->receiptHash));
    text(w, "derived_iq_sha256", hex(f.derived->payloadHash));
    text(w, "derived_plan_json", f.derived->planJson); text(w, "extraction_receipt_json", f.derived->receiptJson);
    w.Key("parent_byte_ranges_raw_reference_filtered"); w.StartArray();
    for (const auto& range : f.derived->parentByteRanges) { w.StartArray();
      w.String(std::to_string(range[0]).c_str()); w.String(std::to_string(range[1]).c_str()); w.EndArray(); }
    w.EndArray();
  }
  w.Key("source_block_sha256_in_BlockKind_order"); w.StartArray(); for (const auto& hash : f.blockHashes) w.String(hex(hash).c_str()); w.EndArray();
  w.Key("actual_gain_db"); w.StartArray(); for (auto value : s.actualGainDb) w.Double(value); w.EndArray();
  text(w, "convention", "y*conj(x_delayed);forward_DFT;positive_exp_i2pi_f_t;excess_path_c_over_fs");
  text(w, "window", "rectangular_correlation_blocks_and_slow_time");
  text(w, "normalization", "CAF_forward_unnormalized;inverse_divides_rows;profiles_unmodified_gauge");
  text(w, "delay_origin", "excess_sample_delay_at_first_last_block_midpoint");
  number(w, "heterodyne_center_hz", f.caf[0].doppler[rows / 2]);
  integer(w, "block_midpoint_offset_twice_samples", correlation - 1);
  w.Key("blocks"); w.StartArray(); for (uint32_t b = 0; b < rows; ++b) {
    w.StartObject(); integer(w, "first", f.firstSample + uint64_t(b) * correlation);
    integer(w, "end", f.firstSample + uint64_t(b + 1) * correlation);
    w.Key("owned"); w.Bool(e.ownedRows[b]); w.EndObject(); } w.EndArray();
  integer(w, "anchor_channel", f.anchorChannelId); number(w, "pilot_coherence", f.pilot->coherence);
  number(w, "minimum_pilot_coherence", f.minimumPilotCoherence);
  w.Key("pilot_correlation"); complex(w, f.pilot->normalizedCorrelation);
  w.Key("gauge_correction_not_applied_to_export"); complex(w, f.gaugeCorrection);
  complexArray(w, "fixed_spatial_coefficients_not_applied_to_export", f.spatialCoefficients);
  integer(w, "spatial_trials", f.declaredSpatialTrials); integer(w, "complex_mac_bound", f.complexMacBound);
  integer(w, "retained_byte_bound", f.retainedBytes);
  text(w, "producer_seal", f.producerSeal); integer(w, "producer_hop_samples", f.hopSamples);
  number(w, "producer_delay_drift_per_hz", f.delayDriftPerHz);
  w.Key("producer_configuration_sha256"); w.StartArray();
  for (const auto& p : f.producers) { w.String(p.configurationSha256.c_str()); } w.EndArray();
  w.Key("channels"); w.StartArray(); for (size_t ch = 0; ch < f.caf.size(); ++ch) {
    const auto& caf = f.caf[ch]; const auto& migration = f.migration[ch];
    w.StartObject(); integer(w, "physical_channel", f.channelIds[ch]);
    w.Key("delay_samples"); w.StartArray(); for (auto d : caf.delay) w.Int(d); w.EndArray();
    w.Key("doppler_hz"); w.StartArray(); for (auto d : caf.doppler) w.Double(d); w.EndArray();
    w.Key("caf_doppler_major"); w.StartArray(); for (const auto& row : caf.data) for (auto z : row) complex(w, z); w.EndArray();
    complexArray(w, "range_slow_time_uncropped", migration.rangeSlowTime);
    integer(w, "common_first_column", migration.firstColumn); integer(w, "common_columns", migration.columns);
    text(w, "mask", "common_contiguous_crop_no_padding"); text(w, "interpolator", "DC_preserving_Lanczos8_v1");
    w.Key("rates"); w.StartArray(); for (const auto& bank : migration.banks) {
      w.StartObject(); number(w, "delay_rate_bins_per_second", bank.delayRateBinsPerSecond);
      number(w, "white_range_noise_gain", bank.whiteNoiseGain);
      complexArray(w, "caf_doppler_major", bank.cells); complexArray(w, "range_slow_time", bank.rangeSlowTime);
      w.Key("kernels_by_block"); w.StartArray(); for (const auto& kernel : bank.interpolationKernels) {
        w.StartObject(); text(w, "first_offset", std::to_string(kernel.first)); w.Key("real_weights"); w.StartArray();
        for (auto weight : kernel.weights) { w.Double(weight); }
        w.EndArray(); w.EndObject(); } w.EndArray();
      w.EndObject(); } w.EndArray(); w.EndObject(); } w.EndArray(); w.EndObject();
}
}
int main(int argc, char** argv) try {
  const auto exe = executableHash(argv[0]);
  std::optional<std::filesystem::path> iqPlan, iqReceipt;
  blah2::resident_replay::Hash planHash{}, receiptHash{};
  if (argc > 5 && std::string(argv[1]) == "--iq") {
    iqPlan = argv[2]; iqReceipt = argv[3]; planHash = hashArgument(argv[4]); receiptHash = hashArgument(argv[5]);
    argv += 5; argc -= 5;
    if (!fftw_init_threads()) throw std::runtime_error("FFTW initialization failed");
    blah2::set_fftw_planner_threads(1);
  }
  if (argc < (iqPlan ? 14 : 15) || argc > (iqPlan ? 21 : 22) || std::string(argv[1]) != "--channels" || std::string(argv[3]) != "--anchor" ||
      std::string(argv[5]) != "--projection" || std::string(argv[7]) != "--rates" ||
      std::string(argv[9]) != "--delay" || std::string(argv[11]) != "--residual-hz")
    throw std::invalid_argument("Usage: passiveIsarApertureInspect [--iq PLAN RECEIPT PLAN_SHA RECEIPT_SHA] --channels IDS --anchor ID --projection ID --rates 0,RATE,... --delay SAMPLE --residual-hz F0,F1,F2,... FRAME... (2..9 native frames: bootstrap plus 1..8 products; or 1..8 unintegrated calibration IQ slices)");
  blah2::integration::DirectPathCoherentExperiment::Config config;
  for (double channel : list(argv[2])) config.channelIds.push_back(id(channel));
  const auto anchor = list(argv[4]), projection = list(argv[6]), delay = list(argv[10]);
  if (anchor.size() != 1 || projection.size() != 1 || delay.size() != 1) throw std::invalid_argument("Expected scalar");
  config.anchorChannelId = id(anchor[0]); config.migrationRates = list(argv[8]);
  config.spatialCoefficients.assign(config.channelIds.size(), 0.);
  const auto selected = std::find(config.channelIds.begin(), config.channelIds.end(), id(projection[0]));
  if (selected == config.channelIds.end()) throw std::invalid_argument("Projection channel absent");
  config.spatialCoefficients[size_t(selected - config.channelIds.begin())] = 1.;
  const auto frequencies = list(argv[12]);
  if (!iqPlan) {
    config.retainApertureProducts = true;
    config.apertureRetentionMaximumBytes = config.apertureRetentionRemainingBytes = 128u << 20;
    config.apertureRetentionRemainingWork = blah2::integration::RangeMigrationReplay::maximumMacs;
  }
  cr::ExperimentReader reader(config); cr::PassiveIsarAperture aperture;
  std::optional<cr::Sidecar> bootstrap;
  // Complete admission before emitting potentially large arrays.
  for (int i = 13; i < argc; ++i) {
    if (iqPlan) {
      auto segment = cr::loadAuthenticatedIqSegment(*iqPlan, *iqReceipt, argv[i], planHash, receiptHash);
      if (config.channelIds != std::vector<uint32_t>{segment.channelId} || config.anchorChannelId != segment.channelId)
        throw std::invalid_argument("Derived IQ channel/anchor differs from authenticated extraction");
      aperture.append(reader.aperture(product(std::move(segment)), aperture.remainingWork(), aperture.remainingBytes()));
    } else {
      const uint64_t bytes = config.apertureRetentionMaximumBytes - ((256u << 20) - aperture.remainingBytes());
      reader.setApertureExecutionAllowance(bytes, reader.remainingApertureWork());
      const auto results = reader.appendAll(argv[i]); const auto retained = reader.apertureProduct();
      if (!bootstrap) { bootstrap = results.front().source;
        if (retained) throw std::logic_error("Bootstrap unexpectedly published an aperture");
      } else {
        if (!retained) throw std::logic_error("Native producer handoff unavailable");
        aperture.append(reader.aperture(*retained, std::min(aperture.remainingWork(), reader.remainingApertureWork()), bytes));
      }
    }
  }
  std::vector<cr::PassiveIsarAperture::Spectrum> spectra;
  for (uint32_t rate = 0; rate < config.migrationRates.size(); ++rate)
    spectra.push_back(aperture.spectrum(rate, delay[0], frequencies));
  rapidjson::OStreamWrapper stream(std::cout);
  for (const auto& entry : aperture.entries()) { Writer writer(stream); frameRecord(writer, entry, exe); std::cout << '\n'; }
  Writer w(stream); w.StartObject(); text(w, "schema", "vectorwarp-passive-isar-aperture-v1"); text(w, "record", "complete");
  w.Key("offline_only"); w.Bool(true); w.Key("null_calibrated"); w.Bool(false); w.Key("metric_image"); w.Bool(false);
  text(w, "handoff", iqPlan ? "unintegrated_authenticated_derived_calibration_diagnostic" : "canonical_pre_winner_coherent_producer");
  if (bootstrap) { integer(w, "bootstrap_first_sample", bootstrap->firstSample); integer(w, "bootstrap_end_sample", bootstrap->endSample); }
  integer(w, "frames", aperture.entries().size()); integer(w, "unique_blocks", aperture.uniqueBlocks());
  integer(w, "missing_support_samples", aperture.missingSupportSamples());
  integer(w, "diagnostic_windows", 1); integer(w, "evaluated_diagnostic_cells", spectra.size() * frequencies.size());
  text(w, "diagnostic", "constant_translation_residual_Doppler_signature_not_scaled_cross_range");
  w.Key("spectra"); w.StartArray(); for (const auto& s : spectra) {
    w.StartObject(); integer(w, "rate_index", s.rateIndex); integer(w, "per_cell_trials", s.perCellTrials);
    integer(w, "complex_mac_bound", s.complexMacBound); number(w, "delay_at_first_midpoint", s.delayAtFirstMidpoint);
    number(w, "translation_doppler_hz", s.translationDopplerHz);
    w.Key("residual_doppler_hz"); w.StartArray(); for (double f : s.residualDopplerHz) w.Double(f); w.EndArray();
    complexArray(w, "amplitude_rectangular_sum_over_sqrt_unique_blocks", s.amplitude); w.EndObject(); } w.EndArray(); w.EndObject();
  std::cout << '\n'; if (!std::cout) throw std::runtime_error("Aperture export write failed"); return 0;
} catch (const std::exception& error) { std::cerr << "passiveIsarApertureInspect: " << error.what() << '\n'; return 1; }
