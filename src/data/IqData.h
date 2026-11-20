/// @file IqData.h
/// @class IqData
/// @brief A class to store IQ data.
/// @details Implements a FIFO queue to store IQ samples.
/// @author 30hours

#ifndef IQDATA_H
#define IQDATA_H

#include <stdint.h>
#include <deque>
#include <vector>
#include <complex>
#include <mutex>
#include "FloatCaptureBlocks.h"
#include "IqSampleView.h"

class IqData
{
private:
  /// @brief Maximum number of samples.
  uint32_t n;

  /// @brief True if should not push to buffer (mutex).
  std::mutex mutex_lock;

  /// @brief Pointer to IQ data.
  std::deque<std::complex<double>> *data;

  // Canonical DSP opts into retained contiguous FP64 storage. Legacy mutable
  // deque access materializes once and transfers authority back to the FIFO.
  mutable bool contiguousActive = false;
  std::vector<std::complex<double>> contiguousData;
  std::size_t contiguousCount = 0;
  void materialize_deque() const;

  // Live wideband GPU capture is received as FP32 and consumed as FP32.
  // Keeping that queue native avoids a lossless-but-expensive FP32->FP64->FP32
  // round trip. The ordinary DSP/storage representation remains FP64.
  bool floatCapture = false;
  std::unique_ptr<FloatCaptureStorage> floatCaptureData;

  /// @brief Samples retired because this bounded queue overflowed.
  uint64_t droppedSamples = 0;

  /// @brief Minimum value.
  double min;

  /// @brief Maximum value.
  double max;

  /// @brief Mean value.
  double mean;

  /// @brief Spectrum vector.
  std::vector<std::complex<double>> spectrum;

  /// @brief Frequency vector (Hz).
  std::vector<double> frequency;

public:
  struct StorageMetrics {
    uint64_t allocations = 0, materializations = 0, materializedSamples = 0;
    double materializationMs = 0;
  };
private:
  mutable StorageMetrics storageMetrics;
public:
  IqSampleView sample_view() const;
  // Preserves existing samples, including partial CPIs. Retired storage stays
  // initialized; callers overwrite newly admitted samples and must not resize
  // the returned vector. Views expire on mutation or legacy deque access.
  // Capture FIFOs never opt into this mode.
  std::vector<std::complex<double>>& resize_contiguous_for_write(uint32_t count);
  StorageMetrics take_storage_metrics();
  /// @brief Constructor.
  /// @param n Number of samples.
  /// @return The object.
  IqData(uint32_t n);

  /// @brief Destructor.
  ~IqData();

  IqData(const IqData&) = delete;
  IqData& operator=(const IqData&) = delete;

  /// @brief Getter for maximum number of samples.
  /// @return Maximum number of samples.
  uint32_t get_n();

  /// @brief Change the producer capacity while preserving all queued samples.
  /// @warning Caller must hold this object's lock. Shrinking below the current
  /// queue length is rejected instead of silently retiring samples.
  void set_capacity_unlocked(uint32_t capacity);

  /// @brief Getter for current data length.
  /// @return Number of samples currently in data.
  uint32_t get_length();

  /// @brief Cumulative overflow retirement count; caller owns queue locking.
  uint64_t get_dropped_samples() const;

  /// @brief Locker for mutex.
  /// @return Void.
  void lock();

  /// @brief Unlocker for mutex.
  /// @return Void.
  void unlock();

  /// @brief Getter for data.
  /// @return IQ data.
  std::deque<std::complex<double>> get_data();

  /// @brief Legacy read-only FIFO access; contiguous DSP storage materializes
  /// once. Caller must prevent concurrent access and subsequent mutation.
  const std::deque<std::complex<double>>& view_data() const;

  /// @brief Remove and return a block from the front of the queue.
  std::deque<std::complex<double>> drain_front(uint32_t count);

  /// @brief Transfer the FP64 FIFO, or copy/convert native FP32 samples.
  /// @warning Caller must hold this object's lock.
  std::deque<std::complex<double>> take_all();

  /// @brief Opt an empty producer FIFO into native FP32 capture storage.
  /// @warning Must be called before the capture producer starts.
  void enable_float_capture(uint32_t blockSamples = 0, uint32_t warmBlocks = 0);

  /// @brief Copy/empty the native FP32 FIFO for legacy/diagnostic callers.
  /// @warning Caller must hold this object's lock and enable_float_capture().
  std::deque<std::complex<float>> take_all_float();

  /// @brief Transfer completed reusable FP32 blocks without copying samples.
  /// @details Retains a partial producer block unless needed for minimumSamples.
  /// @warning Caller holds the producer lock. Returned blocks own their storage.
  FloatCaptureBlockBatch take_float_blocks(std::size_t minimumSamples = 0);

  FloatCaptureBlockPool::Stats float_capture_pool_stats() const;
  void prepare_float_capture_reserve();
  FloatCaptureBlockPool::ReserveStats float_capture_reserve_stats() const;

  /// @brief Discard a processed block without returning or copying samples.
  /// @warning Caller must prevent concurrent mutation.
  void discard_front(uint32_t count);

  /// @brief Subtract a validated FP32 clutter estimate in FP64 and keep its CPI.
  /// @warning Caller owns this block and has validated the complete estimate.
  void subtract_clutter(const std::complex<float>* estimate, uint32_t count);

  /// @brief Subtract a FP64 clutter estimate divided by its FFT length in place.
  /// @warning Caller owns this block and has validated the complete estimate.
  void subtract_clutter(const std::complex<double>* estimate, uint32_t count,
                        uint32_t divisor);

  /// @brief Replace all samples with an existing block.
  void replace(std::deque<std::complex<double>>&& samples);

  /// @brief Set the writable block length, bounded by the FIFO capacity.
  /// @warning Caller must have exclusive access while writing returned samples.
  std::deque<std::complex<double>>& resize_for_write(uint32_t count);

  /// @brief Push a sample to the queue.
  /// @param sample A single sample.
  /// @return Void.
  void push_back(std::complex<double> sample);

  /// @brief Append a coherent float32 block while retaining the newest n.
  /// @warning Caller must hold this object's lock.
  void append_unlocked(const std::vector<std::complex<float>>& samples);

  /// @brief Append an existing receive block without a temporary vector.
  /// @warning Caller holds the lock; like the vector overload, retains newest n.
  void append_unlocked(const std::complex<float>* samples, std::size_t count);

  /// @brief Replace two coherent buffers from signed16 IIQQ capture storage.
  /// @warning Caller exclusively owns both buffers.
  void assign_paired_i16(const int16_t* samples, uint32_t count, IqData& other);

  /// @brief Overwrite a complete complex CPI while reusing existing deque storage.
  /// @warning Caller exclusively owns this buffer.
  void assign_complex(const std::complex<double>* samples, uint32_t count);


  /// @brief Pop the front of the queue.
  /// @return Sample from the front of the queue.
  std::complex<double> pop_front();

  /// @brief Print to stdout (debug).
  /// @return Void.
  void print();

  /// @brief Clear samples from the queue.
  /// @return Void.
  void clear();

  /// @brief Update the time differences and names.
  /// @param spectrum Spectrum vector.
  /// @return Void.
  void update_spectrum(std::vector<std::complex<double>> spectrum);

  /// @brief Update the time differences and names.
  /// @param frequency Frequency vector.
  /// @return Void.
  void update_frequency(std::vector<double> frequency);

  /// @brief Generate JSON of the signal and metadata.
  /// @param timestamp Current time (POSIX ms).
  /// @return JSON string.
  std::string to_json(uint64_t timestamp);
};

#endif
