/// @file RspDuo.h
/// @class RspDuo
/// @brief A class to capture data on the SDRplay RspDuo.
/// @details Loosely based upon the sdrplay_api_sample_app.c and sdr_play.c examples
/// provided in the SDRplay API V3 documentation.
/// This should be read in conjuction with that documentation
///
/// For coherent operation the use of sdrplay_api_Tuner_Both is most important
/// This clue was provided by Gustaw Mazurek of WUT
/// <https://github.com/fventuri/gr-sdrplay/issues/2>
/// <https://github.com/g4eev/RSPduoEME/blob/main/rspduointerface.cpp>
///
/// Reference for using C style callback API with a C++ wrapper:
/// <https://stackoverflow.com/questions/63768893/pointer-problem-using-functions-from-non-object-api-in-objects?rq=1>
/// @author 30hours
/// @author Michael P
/// @todo Remove max time.

#ifndef RSPDUO_H
#define RSPDUO_H

#include "sdrplay_api.h"
#include "capture/Source.h"
#include "data/IqData.h"
#include "SdkSampleClock.h"

#include <stdint.h>
#include <atomic>
#include <array>
#include <mutex>
#include <string>
#include <vector>

#define BUFFER_SIZE_NR 1024

class RspDuo : public Source
{
  friend struct RspDuoTestAccess;
private:
  static constexpr unsigned int MAX_CALLBACK_SAMPLES = 262144;
  std::string requestedSerial;
  std::mutex lifecycleMutex;
  mutable std::mutex receiptMutex;
  struct StartupStages {
    bool settingsValidated = false;
    bool open = false, apiVersion = false, lock = false, enumerate = false;
    bool select = false, unlock = false, debugEnable = false;
    bool getDeviceParams = false, init = false, gainUpdateA = false, gainUpdateB = false;
    float sdkVersion = 0;
    std::string selectedSerial;
    unsigned deviceIndex = 0, hardwareVersion = 0;
  } startupStages;
  bool apiOpened = false;
  bool apiLocked = false;
  bool deviceSelected = false;
  bool deviceInitialized = false;
  std::atomic<bool> deviceRemoved{false};
  std::atomic<bool> callbackFault{false};
  std::atomic<bool> streamEstablished{false};
  // Storage is allocated with the receiver, never by the vendor callback.
  // Pair state belongs to this receiver so one instance cannot consume
  // another instance's tuner-A block.
  std::array<std::vector<short>, 2> callbackStorage;
  std::mutex callbackMutex;
  // Serialize B-side conversion and publication in SDK delivery order while
  // allowing A to fill the other preallocated pair slot.
  std::mutex callbackBProcessingMutex;
  int callbackSlot = -1;
  std::array<bool, 2> callbackSlotBusy{{false, false}};
  unsigned int callbackSamples = 0;
  uint32_t callbackFirstSample = 0;
  uint32_t expectedFirstSample = 0;
  bool expectedFirstSampleValid = false;
  bool scaledSampleCounter = false;
  SdkSampleClock sampleClock;
  IqData* outputBuffer1 = nullptr;
  IqData* outputBuffer2 = nullptr;
  void signal_callback_fault(const char* discontinuity = nullptr) noexcept;
  void cleanup_api() noexcept;
  /// @brief AGC bandwidth (Hz)
  int agc_bandwidth_nr;
  /// @brief AGC set point (dBfs)
  int agc_set_point_nr;
  /// @brief Gain reduction (dB).
  int gain_reduction_nr_a;
  int gain_reduction_nr_b;
  /// @brief LNA state
  int lna_state_nr;
  /// @brief Decimation factor (integer).
  int nDecimation;
  /// @brief MW and FM notch filters.
  bool rf_notch_fg;
  /// @brief DAB notch filter.
  bool dab_notch_fg;
  /// @brief USB bulk transfer mode.
  bool usb_bulk_fg;
  /// @brief SDRplay IF bandwidth enum.
  sdrplay_api_Bw_MHzT bwType;
  /// @brief SDRplay IF mode enum.
  sdrplay_api_If_kHzT ifType;

  /// @brief Maximum frequency (Hz).
  static const double MAX_FREQUENCY_NR;
  /// @brief Minimum AGC set point.
  static const int MIN_AGC_SET_POINT_NR;
  /// @brief Minimum gain reduction.
  static const int MIN_GAIN_REDUCTION_NR;
  /// @brief Maximum gain reduction.
  static const int MAX_GAIN_REDUCTION_NR;
  /// @brief Default sample rate.
  static const int DEF_SAMPLE_RATE_NR;

  /// @brief Check parameters for valid for capture device.
  /// @return The object.
  void validate();

  /// @brief Start API functions.
  /// @return The object.
  void open_api();

  /// @brief Device selection function.
  /// @return The object.
  void get_device();

  /// @brief Set device parameters.
  /// @return The object.
  void set_device_parameters();

  /// @brief Wrapper for C style callback function for stream_a_callback().
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _stream_a_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext)
  {
    auto* receiver = static_cast<RspDuo*>(cbContext);
    if (!receiver) return;
    try { receiver->stream_a_callback(xi, xq, params, numSamples, reset, cbContext); }
    catch (...) { receiver->signal_callback_fault("RSPduo stream A callback metadata failure"); }
  };

  /// @brief Wrapper for C style callback function for stream_b_callback().
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _stream_b_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext)
  {
    auto* receiver = static_cast<RspDuo*>(cbContext);
    if (!receiver) return;
    try { receiver->stream_b_callback(xi, xq, params, numSamples, reset, cbContext); }
    catch (...) { receiver->signal_callback_fault("RSPduo stream B callback failure"); }
  };

  /// @brief Wrapper for C style callback function for event_callback().
  /// @param eventId As defined in SDRplay API.
  /// @param tuner As defined in SDRplay API.
  /// @param params As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  static void _event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext)
  {
    auto* receiver = static_cast<RspDuo*>(cbContext);
    if (!receiver) return;
    try { receiver->event_callback(eventId, tuner, params, cbContext); }
    catch (...) { receiver->signal_callback_fault("RSPduo event callback failure"); }
  };

  /// @brief Tuner a callback as defined in SDRplay API.
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void stream_a_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);

  /// @brief Tuner b callback as defined in SDRplay API.
  /// @param xi Pointer to real part of sample.
  /// @param xq Pointer to imag part of sample.
  /// @param params As defined in SDRplay API.
  /// @param numSamples Number of samples in block.
  /// @param reset As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void stream_b_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);

  /// @brief Event callback function as defined in SDRplay API.
  /// @param eventId As defined in SDRplay API.
  /// @param tuner As defined in SDRplay API.
  /// @param params As defined in SDRplay API.
  /// @param cbContext As defined in SDRplay API.
  /// @return Void.
  void event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);

  /// @brief Start running capture callback function.
  /// @return Void.
  void initialise_device();

  /// @brief Stop running capture callback function.
  /// @return Void.
  void uninitialise_device();

public:
  /// @brief Constructor.
  /// @param fc Center frequency (Hz).
  /// @param path Path to save IQ data.
  /// @return The object.
  RspDuo(std::string type, uint32_t fc, uint32_t fs, 
    std::string path, bool *saveIq, int agcSetPoint, 
    int bandwidthNumber, int gainReductionA, int gainReductionB, 
    int lnaState, bool dabNotch, bool rfNotch, std::string serial = "");

  /// @brief Implement capture function on RSPduo.
  /// @param buffer1 Pointer to reference buffer.
  /// @param buffer2 Pointer to surveillance buffer.
  /// @return Void.
  void process(IqData *buffer1, IqData *buffer2);

  /// @brief Get file name from path.
  /// @return String of file name based on current time.
  std::string set_file(std::string path);

  /// @brief Call methods to start capture.
  /// @return Void.
  void start();

  /// @brief Call methods to gracefully stop capture.
  /// @return Void.
  void stop();

  /// SDK call acceptance only; the API offers no independent tuner readback.
  std::string startup_receipt_json() const;


};

#endif
