/// Wiener-Hopf clutter cancellation with reusable full/block FFT workspaces.
#pragma once
#include "data/IqData.h"
#include <complex>
#include <cstdint>
#include <memory>

class WienerHopf {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  struct FilteredView {
    const std::complex<double>* rotatedReference;
    const std::complex<double>* filteredSurveillance;
    uint32_t samples;
    int32_t delayMin;
    uint64_t generation;
  };
  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
  ~WienerHopf();
  WienerHopf(const WienerHopf&) = delete;
  WienerHopf& operator=(const WienerHopf&) = delete;
  uint32_t filter_fft_length() const;
  // Includes the caller's FFT slot plus persistent correlation/FIR workers.
  uint32_t cpu_worker_slots() const;
  bool process(IqData* reference, IqData* surveillance, bool borrowOutput = false);
  // Read a bounded immutable CPI directly into this instance's persistent
  // workspaces.  The input is never modified.
  // This entrypoint has no writable IqData owner, so borrowOutput must be true.
  bool process_borrowed_input(const std::complex<double>* reference,
    uint32_t referenceSamples, const std::complex<double>* surveillance,
    uint32_t surveillanceSamples, bool borrowOutput = true);
  // Decode bounded IIQQ signed16 input directly into the persistent reference
  // and surveillance workspaces.  referenceChannel is 0 or 1.
  bool process_borrowed_paired_i16(const int16_t* input,
    uint32_t sampleCount, uint32_t referenceChannel);
  FilteredView filtered_view() const;
};
