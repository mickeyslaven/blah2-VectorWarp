#pragma once

#include "data/CanonicalSignal.h"
#include "FrontendPlanContract.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace blah2::iq {

// Immutable ingress policy, distinct from downstream RadarPlan. Backend,
// receiver, live/replay selection and truth cannot select this identity.
class FrontendPlan {
  const std::string identity_;
  const uint32_t ratio_, groupDelay_, channels_;
  const uint64_t center_;
  const bool channelized_;
  const std::vector<uint32_t> references_, surveillance_;
  static std::string serialize(uint64_t center, uint32_t rate,
      std::size_t channels, const SignalDefinition& lane, bool channelized,
      const std::vector<uint32_t>& references, const std::vector<uint32_t>& surveillance) {
    const uint32_t output = lane.outputRateHz;
    if (!center || !rate || !output || rate % output || !lane.frequencyHz ||
        !channels || channels > 8 || center > 9007199254740991ULL ||
        lane.frequencyHz > 9007199254740991ULL)
      throw std::invalid_argument("Invalid canonical frontend plan");
    const uint64_t ratio = rate / output;
    const uint64_t taps = channelized ? lane.tapsPerPhase : 0;
    const uint64_t count = channelized ? 2 * ((taps * ratio + 1) / 2) + 1 : 1;
    const uint64_t occupied = channelized ? lane.occupiedBandwidthHz : rate;
    const uint64_t offset = lane.frequencyHz > center ? lane.frequencyHz - center : center - lane.frequencyHz;
    if ((channelized && (!occupied || occupied >= output || taps < 4 || taps > 128 ||
          count > 131071 || 2 * offset + occupied >= rate)) ||
        (!channelized && (output != rate || lane.frequencyHz != center)))
      throw std::invalid_argument("Invalid canonical frontend extraction");
    rapidjson::Document d;
    d.Parse(frontendPlanContract);
    const auto number = [&](const char* key, uint64_t value) { d[key].SetUint64(value); };
    if (channelized) {
      d["extraction"].SetString("channelized");
      d["filter"].SetString("frequency-shift-hamming-sinc-unit-dc-v1");
      d["standard"].SetString(lane.standard.c_str(), d.GetAllocator());
    }
    number("source_sample_rate_hz", rate); number("source_center_hz", center);
    number("output_sample_rate_hz", output); number("frequency_hz", lane.frequencyHz);
    number("occupied_bandwidth_hz", occupied); number("filter_taps_per_phase", taps);
    number("filter_tap_count", count); number("input_samples_per_output", ratio);
    number("transition_width_hz_x2", channelized ? output - occupied : 0);
    number("cutoff_hz_x4", channelized ? output + occupied : 0);
    number("group_delay_input_samples", (count - 1) / 2);
    for (uint32_t channel = 0; channel < channels; ++channel)
      d["channel_ids"].PushBack(channel, d.GetAllocator());
    const auto mapping = [&](const char* key, const std::vector<uint32_t>& ids) {
      std::vector<bool> seen(channels);
      if (ids.empty()) throw std::invalid_argument("Empty canonical frontend channel mapping");
      for (const auto channel : ids) {
        if (channel >= channels || seen[channel])
          throw std::invalid_argument("Invalid canonical frontend channel mapping");
        seen[channel] = true; d[key].PushBack(channel, d.GetAllocator());
      }
    };
    mapping("reference_channel_ids", references); mapping("surveillance_channel_ids", surveillance);
    rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
    d.Accept(w); return {b.GetString(), b.GetSize()};
  }
public:
  FrontendPlan(uint64_t center, uint32_t rate, std::size_t channels,
      const SignalDefinition& lane, bool channelized,
      std::vector<uint32_t> references, std::vector<uint32_t> surveillance)
    : identity_(serialize(center, rate, channels, lane, channelized, references, surveillance)),
      ratio_(rate / lane.outputRateHz), groupDelay_(channelized ?
        (uint64_t(lane.tapsPerPhase) * ratio_ + 1) / 2 : 0), channels_(channels),
      center_(center), channelized_(channelized), references_(std::move(references)),
      surveillance_(std::move(surveillance)) {}
  const std::string& identity() const { return identity_; }
  uint64_t sourceCenterHz() const { return center_; }
  bool isChannelized() const { return channelized_; }
  void validate(uint32_t rate, std::size_t channels, const SignalDefinition& lane) const {
    if (identity_ != serialize(center_, rate, channels, lane, channelized_, references_, surveillance_))
      throw std::invalid_argument("Signal definition differs from its immutable frontend plan");
  }
  void validateRoles(const std::vector<uint32_t>& references, const std::vector<uint32_t>& surveillance) const {
    if (references_ != references || surveillance_ != surveillance)
      throw std::invalid_argument("Canonical frontend channel mapping changed");
  }
  void validate(const CoherentSamples& samples) const {
    samples.validateStorage();
    if (samples.channelCount() != channels_ || samples.inputSamplesPerOutput != ratio_ ||
        samples.groupDelayInputSamples != groupDelay_ ||
        (samples.hasFirstInputSample && samples.firstInputSample % ratio_))
      throw std::invalid_argument("Canonical signal differs from its immutable frontend plan");
  }
};
}
