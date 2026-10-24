#include "process/fusion/CoherentBeamBank.h"
#include "../../../core/TrackBeforeDetect.private.h"

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using rapidjson::Value;

const Value& member(const Value& object, const char* name) {
  if (!object.IsObject() || !object.HasMember(name))
    throw std::invalid_argument(std::string("Missing native replay field: ") + name);
  return object[name];
}

double number(const Value& object, const char* name) {
  const auto& value = member(object, name);
  if (!value.IsNumber() || !std::isfinite(value.GetDouble()))
    throw std::invalid_argument(std::string("Invalid native replay number: ") + name);
  return value.GetDouble();
}

uint32_t positiveUint(const Value& object, const char* name) {
  const auto& value = member(object, name);
  if (!value.IsUint() || !value.GetUint())
    throw std::invalid_argument(std::string("Invalid native replay integer: ") + name);
  return value.GetUint();
}

uint32_t nonnegativeUint(const Value& object, const char* name) {
  const auto& value = member(object, name);
  if (!value.IsUint())
    throw std::invalid_argument(std::string("Invalid native replay integer: ") + name);
  return value.GetUint();
}

std::string text(const Value& object, const char* name) {
  const auto& value = member(object, name);
  if (!value.IsString() || !value.GetStringLength())
    throw std::invalid_argument(std::string("Invalid native replay text: ") + name);
  return value.GetString();
}

std::complex<double> complexValue(const Value& value) {
  if (!value.IsArray() || value.Size() != 2 || !value[0].IsNumber() ||
      !value[1].IsNumber() || !std::isfinite(value[0].GetDouble()) ||
      !std::isfinite(value[1].GetDouble()))
    throw std::invalid_argument("Invalid native replay complex value");
  return {value[0].GetDouble(), value[1].GetDouble()};
}

std::vector<std::complex<double>> complexVector(const Value& value,
  uint32_t channels) {
  if (!value.IsArray() || value.Size() != channels)
    throw std::invalid_argument("Native replay channel vector is incomplete");
  std::vector<std::complex<double>> result;
  result.reserve(channels);
  for (const auto& item : value.GetArray()) result.push_back(complexValue(item));
  return result;
}

std::vector<std::array<double, 3>> positions(const Value& value,
  uint32_t channels) {
  if (!value.IsArray() || value.Size() != channels)
    throw std::invalid_argument("Native replay antenna geometry is incomplete");
  std::vector<std::array<double, 3>> result;
  for (const auto& row : value.GetArray()) {
    if (!row.IsArray() || row.Size() != 3)
      throw std::invalid_argument("Native replay antenna position is invalid");
    std::array<double, 3> position{};
    for (rapidjson::SizeType index = 0; index < 3; ++index) {
      if (!row[index].IsNumber() || !std::isfinite(row[index].GetDouble()))
        throw std::invalid_argument("Native replay antenna position is non-finite");
      position[index] = row[index].GetDouble();
    }
    result.push_back(position);
  }
  return result;
}

void powers(rapidjson::Writer<rapidjson::OStreamWrapper>& writer,
  const CoherentBeamBank& bank, const std::vector<std::complex<double>>& channels) {
  writer.StartArray();
  for (double value : bank.powerBank(channels)) writer.Double(value);
  writer.EndArray();
}

void arm(rapidjson::Writer<rapidjson::OStreamWrapper>& writer,
  const char* bankHash, const CoherentBeamBank& bank,
  const Value& cut, const Value& training, uint32_t channels,
  const std::vector<int>& offsets) {
  writer.StartObject();
  writer.Key("bank_sha256"); writer.String(bankHash);
  writer.Key("cut_power_by_beam");
  powers(writer, bank, complexVector(cut, channels));
  writer.Key("training_power_by_offset_beam"); writer.StartArray();
  for (int offset : offsets) {
    const Value* found = nullptr;
    for (const auto& row : training.GetArray())
      if (row.IsObject() && member(row, "offset").IsInt() &&
          member(row, "offset").GetInt() == offset) found = &row;
    if (!found) throw std::invalid_argument("Native replay is missing a CA-CFAR training offset");
    writer.StartObject(); writer.Key("offset"); writer.Int(offset);
    writer.Key("power_by_beam");
    powers(writer, bank, complexVector(member(*found, "channels"), channels));
    writer.EndObject();
  }
  writer.EndArray(); writer.EndObject();
}
}

int main(int argc, char** argv) try {
  if (argc != 3) throw std::invalid_argument(
    "usage: detectionManifoldGridReplay INPUT.json OUTPUT.json");
  std::ifstream input(argv[1]);
  if (!input) throw std::runtime_error("Cannot open native replay input");
  rapidjson::IStreamWrapper inputStream(input);
  rapidjson::Document document; document.ParseStream(inputStream);
  if (document.HasParseError() || !document.IsObject() ||
      text(document, "schema") != "vectorwarp-detection-manifold-native-input-v1" ||
      !member(document, "same_iq").IsTrue() ||
      !member(document, "production_equivalent").IsTrue() ||
      !member(document, "truth_selected_beam").IsFalse())
    throw std::invalid_argument("Invalid native full-bank replay input contract");

  const auto& configuration = member(document, "configuration");
  if (text(configuration, "fusion_mode") != "coherent_beam_bank")
    throw std::invalid_argument(
      "Native replay currently supports only unwhitened coherent_beam_bank fusion");
  const uint32_t channels = positiveUint(configuration, "channels");
  const double frequency = number(configuration, "frequency_hz");
  const auto geometry = positions(member(configuration, "positions_m"), channels);
  const auto reference = complexVector(member(document, "reference_weights"), channels);
  const double directAzimuth = number(configuration, "direct_path_azimuth_rad");
  const double directNull = number(configuration, "direct_path_null_db");
  const auto& banks = member(document, "banks");
  const auto& baselineMetadata = member(banks, "baseline");
  const auto& candidateMetadata = member(banks, "candidate");
  const uint32_t baselineBeams = positiveUint(baselineMetadata, "beams");
  const uint32_t candidateBeams = positiveUint(candidateMetadata, "beams");
  if (baselineBeams != candidateBeams || !member(candidateMetadata, "runtime_compatible").IsTrue())
    throw std::invalid_argument("Native replay requires equal complete runtime beam banks");

  CoherentBeamBank::Config baselineConfig{frequency, baselineBeams, geometry,
    directAzimuth, directNull, {}};
  CoherentBeamBank::Config candidateConfig = baselineConfig;
  const auto& responses = member(candidateMetadata, "responses");
  if (!responses.IsArray() || responses.Size() != candidateBeams)
    throw std::invalid_argument("Native replay dense candidate bank is incomplete");
  for (const auto& response : responses.GetArray()) {
    const auto vector = complexVector(response, channels);
    candidateConfig.measuredResponses.insert(candidateConfig.measuredResponses.end(),
      vector.begin(), vector.end());
  }
  CoherentBeamBank baseline(baselineConfig), candidate(candidateConfig);
  baseline.calibrate(reference); candidate.calibrate(reference);

  const auto& detector = member(document, "detector");
  if (text(detector, "contract") != "vectorwarp-production-ca-cfar-full-bank-v1")
    throw std::invalid_argument("Native replay detector contract is not production CA-CFAR");
  const uint32_t guard = nonnegativeUint(detector, "guard_cells");
  const uint32_t side = positiveUint(detector, "training_cells_per_side");
  if (guard > 127 || side > 127)
    throw std::invalid_argument("Native replay CA-CFAR geometry exceeds production int8 limits");
  const uint32_t integration = positiveUint(detector, "integration_cpis");
  if (integration != 1 || number(detector, "motion_trials") != 1)
    throw std::invalid_argument(
      "Native replay currently supports only single-CPI, no-motion detection");
  const double detectorPfa = number(detector, "detector_pfa");
  std::vector<int> offsets;
  for (int value = -int(guard + side); value < -int(guard); ++value) offsets.push_back(value);
  for (int value = int(guard) + 1; value <= int(guard + side); ++value) offsets.push_back(value);
  const double alpha = track_before_detect::cfarAlpha(detectorPfa,
    uint32_t(offsets.size()), integration);

  const auto& cuts = member(document, "cuts");
  if (!cuts.IsArray() || cuts.Size() < 2)
    throw std::invalid_argument("Native replay requires target and nuisance CUTs");
  std::set<std::string> cutIds; unsigned targets = 0, nuisances = 0;
  for (const auto& cut : cuts.GetArray()) {
    const std::string id = text(cut, "id"), role = text(cut, "role");
    if (!cutIds.insert(id).second || (role != "target" && role != "nuisance"))
      throw std::invalid_argument("Native replay CUT identity is invalid");
    targets += role == "target"; nuisances += role == "nuisance";
    const auto& training = member(cut, "training");
    if (!training.IsArray() || training.Size() != offsets.size())
      throw std::invalid_argument("Native replay CA-CFAR training grid is incomplete");
  }
  if (targets != 1 || !nuisances)
    throw std::invalid_argument("Native replay requires exactly one target and nuisance support");

  const std::filesystem::path outputPath(argv[2]);
  const auto temporaryPath = outputPath.string() + ".tmp";
  std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot open native replay output");
  rapidjson::OStreamWrapper outputStream(output);
  rapidjson::Writer<rapidjson::OStreamWrapper> writer(outputStream);
  writer.SetMaxDecimalPlaces(17); writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-native-detection-full-bank-grid-v1");
  writer.Key("production_equivalent"); writer.Bool(true);
  writer.Key("same_iq"); writer.Bool(true);
  writer.Key("truth_selected_beam"); writer.Bool(false);
  writer.Key("configuration"); configuration.Accept(writer);
  writer.Key("detector"); writer.StartObject();
  for (auto item = detector.MemberBegin(); item != detector.MemberEnd(); ++item) {
    writer.Key(item->name.GetString(), item->name.GetStringLength()); item->value.Accept(writer);
  }
  writer.Key("native_alpha"); writer.Double(alpha); writer.EndObject();
  writer.Key("banks"); writer.StartObject();
  writer.Key("baseline"); writer.StartObject(); writer.Key("sha256");
  writer.String(text(baselineMetadata, "sha256").c_str()); writer.Key("beams"); writer.Uint(baselineBeams);
  writer.Key("descriptor"); writer.StartObject();
  writer.Key("frequency_hz"); writer.Double(frequency); writer.Key("beams"); writer.Uint(baselineBeams);
  writer.Key("positions_m"); member(configuration, "positions_m").Accept(writer);
  writer.Key("direct_path_azimuth_rad"); writer.Double(directAzimuth);
  writer.Key("direct_path_null_db"); writer.Double(directNull); writer.EndObject();
  writer.EndObject();
  writer.Key("candidate"); writer.StartObject(); writer.Key("sha256");
  writer.String(text(candidateMetadata, "sha256").c_str()); writer.Key("beams"); writer.Uint(candidateBeams);
  writer.Key("runtime_compatible"); writer.Bool(true); writer.Key("elevation_range_deg");
  member(candidateMetadata, "elevation_range_deg").Accept(writer);
  // Preserve the exact response matrix consumed above.  The trusted receipt
  // generator compares this matrix with the immutable candidate artifact, so
  // a caller cannot substitute weights while retaining its declared SHA.
  writer.Key("responses"); responses.Accept(writer); writer.EndObject(); writer.EndObject();
  writer.Key("truth_label"); member(document, "truth_label").Accept(writer);
  writer.Key("cuts"); writer.StartArray();
  for (const auto& cut : cuts.GetArray()) {
    writer.StartObject(); writer.Key("id"); writer.String(text(cut, "id").c_str());
    writer.Key("role"); writer.String(text(cut, "role").c_str());
    writer.Key("baseline"); arm(writer, text(baselineMetadata, "sha256").c_str(),
      baseline, member(cut, "cut"), member(cut, "training"), channels, offsets);
    writer.Key("candidate"); arm(writer, text(candidateMetadata, "sha256").c_str(),
      candidate, member(cut, "cut"), member(cut, "training"), channels, offsets);
    writer.EndObject();
  }
  writer.EndArray(); writer.Key("provenance"); member(document, "provenance").Accept(writer);
  writer.EndObject(); output.flush();
  if (!output) throw std::runtime_error("Cannot write native replay output");
  output.close();
  std::error_code error;
  std::filesystem::rename(temporaryPath, outputPath, error);
  if (error) {
    std::filesystem::remove(outputPath, error); error.clear();
    std::filesystem::rename(temporaryPath, outputPath, error);
  }
  if (error) throw std::runtime_error("Cannot atomically publish native replay output");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 2;
}
