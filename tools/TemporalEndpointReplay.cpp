// Bounded, truth-blind replay adapter around the production CPU tracker.
// stdin: one JSON object per endpoint; reset=true starts a new independent run.
#include "process/tracker/Tracker.h"
#include "rapidjson/document.h"
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main() {
  std::unique_ptr<Tracker> tracker;
  std::string line;
  while (std::getline(std::cin, line)) {
    rapidjson::Document d;
    d.Parse(line.c_str());
    if (d.HasParseError() || !d.IsObject()) return 2;
    if (d["reset"].GetBool()) {
      tracker = std::make_unique<Tracker>(d["m"].GetUint(), d["n"].GetUint(),
        d["delete_after"].GetUint(), d["base_cpi"].GetDouble(),
        d["max_acc"].GetDouble(), d["range_resolution_m"].GetDouble(),
        d["wavelength_m"].GetDouble(), d["birth_snr_db"].GetDouble(),
        true, DopplerRidgeConfig{},
        !d.HasMember("legacy_rectangle") || !d["legacy_rectangle"].GetBool(),
        d.HasMember("association_maximum_nis") ?
          d["association_maximum_nis"].GetDouble() : 9.210340371976184,
        BirthExclusivityConfig{
          d.HasMember("birth_exclusivity") && d["birth_exclusivity"].GetBool(),
          d.HasMember("birth_delay_half_width_bins") ?
            d["birth_delay_half_width_bins"].GetDouble() : 1.0,
          d.HasMember("birth_doppler_half_width_bins") ?
            d["birth_doppler_half_width_bins"].GetDouble() : 1.0});
    }
    if (!tracker) return 3;
    std::vector<double> delay, doppler, snr;
    for (const auto& p : d["points"].GetArray()) {
      delay.push_back(p[0].GetDouble());
      doppler.push_back(p[1].GetDouble());
      snr.push_back(p[2].GetDouble());
    }
    Detection detection(delay, doppler, snr);
    const auto time = d["timestamp_ms"].GetUint64();
    auto tracks = tracker->process(&detection, time);
    const auto& stats = tracker->frame_diagnostics();
    std::cout << std::setprecision(17) << "{\"index\":" << d["index"].GetUint64()
      << ",\"associations\":" << stats.associations
      << ",\"pair_in_gate\":" << stats.pairInGate
      << ",\"misses\":" << stats.misses
      << ",\"promotions\":" << stats.promotions
      << ",\"birth_exclusive_rejected\":" << stats.birthExclusiveRejected
      << ",\"births\":" << stats.initiated << ",\"tracks\":[";
    for (uint64_t i=0; i<tracks->get_n(); ++i) {
      if (i) std::cout << ',';
      auto p = tracks->get_current(i);
      std::cout << "{\"id\":\"" << tracks->get_id(i)
        << "\",\"state\":\"" << tracks->get_state(i)
        << "\",\"delay_bins\":" << p.get_delay().front()
        << ",\"doppler_hz\":" << p.get_doppler().front()
        << ",\"measurements\":" << tracks->get_nAssociated(i) << '}';
    }
    std::cout << "]}\n";
  }
}
