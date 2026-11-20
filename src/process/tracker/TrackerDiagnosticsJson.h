#pragma once

#include "process/tracker/Tracker.h"

// Shared host tracker monitor for CPU, portable GPU and resident-front-end
// routes. Pair rejection counters are exclusive; occupancy is a state gauge.
// No evaluation truth enters these products or tracker decisions.
template<class Writer>
void writeTrackerFrameDiagnostics(Writer& writer, const TrackerFrameDiagnostics& d)
{
  writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-cpu-tracker-frame-diagnostics-v1");
  writer.Key("truth_blind"); writer.Bool(true);
  writer.Key("scope"); writer.String("single-illuminator-single-frame");
  writer.Key("pair_semantics"); writer.String("exclusive-ordered-candidate-track-pair-rejections");
  writer.Key("no_op"); writer.Bool(d.noOp);
  const auto count = [&](const char* key, uint64_t value) {
    writer.Key(key); writer.Uint64(value);
  };
  count("detections", d.detections);
  count("tracks_before", d.tracksBefore);
  count("tracks_after", d.tracksAfter);
  count("candidate_track_pairs", d.pairs);
  count("pair_already_claimed", d.pairClaimed);
  count("pair_delay_rejected", d.pairDelayRejected);
  count("pair_doppler_rejected", d.pairDopplerRejected);
  count("pair_innovation_rejected", d.pairInnovationRejected);
  count("pair_spatial_rejected", d.pairSpatialRejected);
  count("pair_in_gate", d.pairInGate);
  count("pair_lineage_rejected", d.pairLineageRejected);
  count("associations", d.associations);
  count("misses", d.misses);
  count("promotions", d.promotions);
  count("tentative_expired", d.tentativeExpired);
  count("inactivity_deleted", d.inactivityDeleted);
  count("birth_candidates", d.birthCandidates);
  count("birth_snr_rejected", d.birthSnrRejected);
  count("birth_ridge_rejected", d.birthRidgeRejected);
  count("birth_doppler_comb_rejected", d.birthDopplerCombRejected);
  count("birth_range_comb_rejected", d.birthRangeCombRejected);
  count("initiated", d.initiated);
  count("lineage_resets", d.lineageResets);
  count("birth_lineage_rejected", d.birthLineageRejected);
  count("birth_capacity_rejected", d.birthCapacityRejected);
  count("independent_credits", d.independentCredits);
  count("credited_source_cpis", d.creditedCpis);
  count("stationary_birth_rejected", d.stationaryBirthRejected);
  count("stationary_birth_capacity_rejected", d.stationaryBirthCapacityRejected);
  count("stationary_quarantined", d.stationaryQuarantined);
  count("stationary_cells", d.stationaryCells);
  count("stationary_cell_capacity_rejected", d.stationaryCapacityRejected);
  writer.EndObject();
}
