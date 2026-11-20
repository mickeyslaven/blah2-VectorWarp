#pragma once

// Source compatibility for existing capture, publication, and replay callers.
// Production orchestration is owned by the canonical pipeline.
#include "process/pipeline/CanonicalRadar.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"

namespace blah2::multi_illuminator {
using pipeline::AdvancedFeatureConfig;
using pipeline::RadarConfig;
using pipeline::RadarFrame;
using pipeline::NativeCafView;
using pipeline::NativeCafObserver;
using pipeline::LiveTemporalIdentity;
using pipeline::validateAdvancedFeatures;
using pipeline::scaleRadarConfigForLane;
using MultiIlluminatorRadar = pipeline::RadarBank;
} // namespace blah2::multi_illuminator
