#include "RspDuo.h"
#include "capture/PairedCpiQueue.h"
#include "SampleSequence.h"
#include "UsbMode.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

// The licensed local source kit has only standard C++ and SDRplay headers.
// Serial text is printable ASCII after validate()/sdk_serial(), but JSON still
// needs to escape quotes and backslashes rather than trust the input bytes.
static std::string json_string(const std::string& value)
{
  static constexpr char hex[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped += '"';
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(character);
    } else if (character < 32 || character >= 127) {
      escaped += "\\u00";
      escaped += hex[character >> 4];
      escaped += hex[character & 15];
    } else escaped += static_cast<char>(character);
  }
  escaped += '"';
  return escaped;
}

static void require_api(sdrplay_api_ErrT result, const char* operation)
{
  if (result != sdrplay_api_Success)
    throw std::runtime_error(std::string("[RspDuo] ") + operation + ": " + sdrplay_api_GetErrorString(result));
}

static std::string sdk_serial(const sdrplay_api_DeviceT& device)
{
  const auto length = strnlen(device.SerNo, sizeof(device.SerNo));
  if (length == 0 || length == sizeof(device.SerNo))
    throw std::runtime_error("[RspDuo] API returned an invalid receiver serial.");
  for (std::size_t i = 0; i < length; ++i)
    if (static_cast<unsigned char>(device.SerNo[i]) < 32 ||
        static_cast<unsigned char>(device.SerNo[i]) > 126)
      throw std::runtime_error("[RspDuo] API returned a non-printable receiver serial.");
  return std::string(device.SerNo, length);
}

// class static constants
const double RspDuo::MAX_FREQUENCY_NR = 2000000000;
const int RspDuo::MIN_AGC_SET_POINT_NR = -72;        // min agc set point
const int RspDuo::MIN_GAIN_REDUCTION_NR = 20;        // min gain reduction
const int RspDuo::MAX_GAIN_REDUCTION_NR = 59;        // max gain reduction
const int RspDuo::DEF_SAMPLE_RATE_NR = 2000000;      // default sample rate

// global variables (SDRPlay)
sdrplay_api_DeviceT *chosenDevice = NULL;
sdrplay_api_DeviceT devs[1023];
sdrplay_api_DeviceParamsT *deviceParams = NULL;
sdrplay_api_ErrT err;
sdrplay_api_CallbackFnsT cbFns;

short max_a_nr = 0;
short max_b_nr = 0;
std::atomic<bool> run_fg{true};
bool stats_fg = true;

// constructor
RspDuo::RspDuo(std::string _type, uint32_t _fc, 
  uint32_t _fs, std::string _path, bool *_saveIq,
  int _agcSetPoint, int _bandwidthNumber, 
  int _gainReductionA, int _gainReductionB, 
  int _lnaState,
  bool _dabNotch, bool _rfNotch, std::string serial)
  : Source(_type, _fc, _fs, _path, _saveIq), requestedSerial(std::move(serial))
{
  std::unordered_map<int, int> decimationMap = {
    {2000000, 1},
    {1000000, 2},
    {500000, 4},
    {250000, 8},
    {125000, 16},
    {62500, 32}
  };
  std::unordered_map<int, sdrplay_api_Bw_MHzT> ifBandwidthMap = {
    {2000000, sdrplay_api_BW_1_536},
    {1000000, sdrplay_api_BW_0_600},
    {500000, sdrplay_api_BW_0_300},
    {250000, sdrplay_api_BW_0_200},
    {125000, sdrplay_api_BW_0_200},
    {62500, sdrplay_api_BW_0_200}
  };
  std::unordered_map<int, sdrplay_api_If_kHzT> ifModeMap = {
    {2000000, sdrplay_api_IF_1_620},
    {1000000, sdrplay_api_IF_1_620},
    {500000, sdrplay_api_IF_1_620},
    {250000, sdrplay_api_IF_1_620},
    {125000, sdrplay_api_IF_1_620},
    {62500, sdrplay_api_IF_1_620}
  };
  nDecimation = decimationMap[fs];
  bwType = ifBandwidthMap[fs];
  ifType = ifModeMap[fs];
  usb_bulk_fg = rspduo_usb_bulk_mode(std::getenv("VECTORWARP_RSPDUO_USB_MODE"));
  const char* counterScale = std::getenv("VECTORWARP_RSPDUO_COUNTER_SCALE");
  if (counterScale && std::strcmp(counterScale, "1") != 0 && std::strcmp(counterScale, "3") != 0)
    throw std::invalid_argument("VECTORWARP_RSPDUO_COUNTER_SCALE must be 1 or 3");
  scaledSampleCounter = counterScale && std::strcmp(counterScale, "3") == 0;
  sampleClock.configure(scaledSampleCounter ? 3 : 1);
  for (auto& slot : callbackStorage)
    slot.resize(static_cast<std::size_t>(MAX_CALLBACK_SAMPLES) * 4);
  agc_bandwidth_nr = _bandwidthNumber;
  agc_set_point_nr = _agcSetPoint;
  // gain_reduction_nr = _gainReduction;
  gain_reduction_nr_a = _gainReductionA;
  gain_reduction_nr_b = _gainReductionB;
  lna_state_nr = _lnaState;
  rf_notch_fg = _rfNotch;
  dab_notch_fg = _dabNotch;
}

void RspDuo::start()
{
  std::lock_guard<std::mutex> lock(lifecycleMutex);
  {
    std::lock_guard<std::mutex> receiptLock(receiptMutex);
    startupStages = {};
  }
  validate();
  {
    std::lock_guard<std::mutex> receiptLock(receiptMutex);
    startupStages.settingsValidated = true;
  }
  run_fg = true;
  deviceRemoved = false;
  callbackFault = false;
  callbackFaultReason = nullptr;
  streamEstablished = false;
  {
    std::lock_guard<std::mutex> pairingLock(callbackMutex);
    pendingHead = pendingCount = 0;
    callbackSlotBusy.fill(false);
    expectedFirstSampleValid = false;
    sampleClock.clear();
  }
  try {
    open_api();
    get_device();
    set_device_parameters();
  } catch (...) {
    cleanup_api();
    throw;
  }
}

void RspDuo::stop()
{
  run_fg = false;
  std::lock_guard<std::mutex> lock(lifecycleMutex);
  uninitialise_device();
}

void RspDuo::process(IqData *_buffer1, IqData *_buffer2)
{
  outputBuffer1 = _buffer1;
  outputBuffer2 = _buffer2;

  std::unique_lock<std::mutex> lock(lifecycleMutex);
  if (!run_fg) return;
  initialise_device();

  // set gain reduction and lna sate
  deviceParams->rxChannelA->tunerParams.gain.gRdB = gain_reduction_nr_a;
  deviceParams->rxChannelA->tunerParams.gain.LNAstate = lna_state_nr;
  deviceParams->rxChannelB->tunerParams.gain.gRdB = gain_reduction_nr_b;
  deviceParams->rxChannelB->tunerParams.gain.LNAstate = lna_state_nr;

  // update gains after initialization
  require_api(sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_A,
    sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None), "Update tuner A gain");
  {
    std::lock_guard<std::mutex> receiptLock(receiptMutex);
    startupStages.gainUpdateA = true;
  }
  require_api(sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_B,
    sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None), "Update tuner B gain");
  {
    std::lock_guard<std::mutex> receiptLock(receiptMutex);
    startupStages.gainUpdateB = true;
  }
  lock.unlock();

  // control loop
  while (run_fg)
  {
    if (stats_fg)
    {
      const auto ackStart = ackStartNs.load(std::memory_order_acquire);
      const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
      std::cerr << "[RspDuo] max_a_nr: " << max_a_nr <<
        " max_b_nr: " << max_b_nr <<
        " gainA=" << gainEventsA.exchange(0) <<
        " gainB=" << gainEventsB.exchange(0) <<
        " overloadA=" << overloadEventsA.exchange(0) <<
        " overloadB=" << overloadEventsB.exchange(0) <<
        " ack_count=" << ackCount.exchange(0) <<
        " ack_errors=" << ackErrorCount.exchange(0) <<
        " ack_max_us=" << ackMaxUs.exchange(0) <<
        " ack_pending_ms=" << (ackStart ? (nowNs - ackStart) / 1000000 : 0) <<
        " ack_tuner=" << ackTuner.load() <<
        " ack_last_status=" << ackLastStatus.load() << std::endl;
      max_a_nr = 0;
      max_b_nr = 0;
    }
    sleep(1);
  }
  if (deviceRemoved) throw std::runtime_error("[RspDuo] Receiver disconnected. Reconnect it and check the SDRplay API service before restarting.");
  if (callbackFault)
    throw std::runtime_error(std::string("[RspDuo] Dual-tuner callback pairing fault: ") +
      (callbackFaultReason.load() ? callbackFaultReason.load() : "unspecified callback failure") +
      ". Input is stopping; restart after checking the SDRplay API service and receiver connection.");
}

void RspDuo::signal_callback_fault(const char* discontinuity) noexcept
{
  // SDK callbacks must never throw into vendor C code.  Stop the process loop
  // and let its owning C++ thread publish the actionable failure instead.
  if (discontinuity) {
    try { recording_discontinuity(discontinuity); }
    catch (...) {}
  }
  if (pairedCpiQueue) {
    try { pairedCpiQueue->discontinuity(); }
    catch (...) {}
  }
  const char* expected = nullptr;
  callbackFaultReason.compare_exchange_strong(expected,
    discontinuity ? discontinuity : "unspecified callback failure");
  callbackFault.store(true, std::memory_order_relaxed);
  run_fg = false;
}

void RspDuo::clear_pending_locked() noexcept
{
  while (pendingCount) {
    callbackSlotBusy[pendingSlots[pendingHead]] = false;
    pendingHead = (pendingHead + 1) % CALLBACK_SLOTS;
    --pendingCount;
  }
  pendingHead = 0;
}

void RspDuo::set_paired_cpi_queue(PairedCpiQueue* queue)
{
  std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
  std::lock_guard<std::mutex> pairing(callbackMutex);
  if (deviceInitialized || pendingCount)
    throw std::logic_error("Paired CPI queue must be configured before RSPduo starts");
  pairedCpiQueue = queue;
}

void RspDuo::validate() {
    // validate decimation
    if (nDecimation != 1 && nDecimation != 2 && nDecimation != 4 &&
        nDecimation != 8 && nDecimation != 16 && nDecimation != 32) {
        std::cerr << "Error: Decimation must be in {1, 2, 4, 8, 16, 32}" << std::endl;
        throw std::invalid_argument("[RspDuo] Unsupported sample rate/decimation.");
    }

    // validate fc
    if (fc < 1 || fc > MAX_FREQUENCY_NR) {
        std::cerr << "Error: Frequency must be between 1 and " << 
          MAX_FREQUENCY_NR << std::endl;
        throw std::invalid_argument("[RspDuo] Frequency must be between 1 Hz and 2 GHz.");
    }

    // validate agc
    if (agc_bandwidth_nr != 0 && agc_bandwidth_nr != 5 && 
      agc_bandwidth_nr != 50 && agc_bandwidth_nr != 100) {
        std::cerr << "Error: AGC bandwidth must be in {0, 5, 50, 100}" << std::endl;
        throw std::invalid_argument("[RspDuo] AGC bandwidth must be 0, 5, 50 or 100 Hz.");
    }
    if (agc_set_point_nr > 0 || agc_set_point_nr < MIN_AGC_SET_POINT_NR) {
        std::cerr << "Error: AGC set point must be between " << 
          MIN_AGC_SET_POINT_NR << " and 0" << std::endl;
        throw std::invalid_argument("[RspDuo] AGC set point must be between -72 and 0 dBfs.");
    }

    // validate LNA
    if (gain_reduction_nr_a < MIN_GAIN_REDUCTION_NR || gain_reduction_nr_a > MAX_GAIN_REDUCTION_NR) {
        std::cerr << "Error: Gain reduction must be between " << MIN_GAIN_REDUCTION_NR << " and " << MAX_GAIN_REDUCTION_NR << std::endl;
        throw std::invalid_argument("[RspDuo] Tuner A gain reduction must be between 20 and 59 dB.");
    }
    if (gain_reduction_nr_b < MIN_GAIN_REDUCTION_NR || gain_reduction_nr_b > MAX_GAIN_REDUCTION_NR) {
        std::cerr << "Error: Gain reduction must be between " << MIN_GAIN_REDUCTION_NR << " and " << MAX_GAIN_REDUCTION_NR << std::endl;
        throw std::invalid_argument("[RspDuo] Tuner B gain reduction must be between 20 and 59 dB.");
    }
    // Published SDRplay API 3.09 section 5, RSPduo 50-ohm ports.
    const int maxLna = fc < 60000000 ? 6 : (fc < 1000000000 ? 9 : 8);
    if (lna_state_nr < 0 || lna_state_nr > maxLna) {
        throw std::invalid_argument("[RspDuo] LNA state is outside the RSPduo frequency-band range.");
    }

    if (requestedSerial.size() >= sizeof(devs[0].SerNo))
        throw std::invalid_argument("[RspDuo] Receiver serial is too long.");
    for (const unsigned char character : requestedSerial)
        if (character < 32 || character > 126)
            throw std::invalid_argument("[RspDuo] Receiver serial must be printable ASCII.");

    // validate notch filters

    // print them out
    std::cerr << "[RspDuo] Print config" << std::endl;
    std::cerr << "fc (Hz)                       : " << fc << std::endl;
    std::cerr << "fs (Hz)                       : " << fs << std::endl;
    std::cerr << "agc_bandwidth_nr (Hz)         : " << agc_bandwidth_nr << std::endl;
    std::cerr << "agc_set_point_nr (dBfs)       : " << agc_set_point_nr << std::endl;
    std::cerr << "gain_reduction_nr_a (dB)      : " << gain_reduction_nr_a << std::endl;
    std::cerr << "gain_reduction_nr_b (dB)      : " << gain_reduction_nr_b << std::endl;
    std::cerr << "lna_state_nr                  : " << lna_state_nr << std::endl;
    std::cerr << "N_DECIMATION                  : " << nDecimation << std::endl;
    std::cerr << "rf_notch_fg                   : " << (rf_notch_fg ? "true" : "false") << std::endl;
    std::cerr << "dab_notch_fg                  : " << (dab_notch_fg ? "true" : "false") << std::endl;
    std::cerr << "usb_bulk_fg                   : " << (usb_bulk_fg ? "true" : "false") << std::endl;
    std::cerr << "stats_fg                      : " << (stats_fg ? "true" : "false") << std::endl;
    std::cerr << "\n";
}

void RspDuo::open_api()
{
  float ver = 0.0;
  require_api(sdrplay_api_Open(), "Open API; check that the SDRplay API service is running");
  apiOpened = true;
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.open = true; }
  require_api(sdrplay_api_ApiVersion(&ver), "Read API version");
  if (std::isfinite(ver) && ver > 0 && ver <= 100) {
    std::lock_guard<std::mutex> lock(receiptMutex);
    startupStages.sdkVersion = ver;
  }
  if (!std::isfinite(ver) || std::abs(ver - SDRPLAY_API_VERSION) > 0.001f)
    throw std::runtime_error("[RspDuo] Installed SDRplay API version does not match this receiver build.");
  {
    std::lock_guard<std::mutex> lock(receiptMutex);
    startupStages.apiVersion = true;
  }
}

void RspDuo::get_device()
{
  unsigned int i;
  unsigned int ndev;
  unsigned int chosenIdx = 0;

  require_api(sdrplay_api_LockDeviceApi(), "Lock API for device selection");
  apiLocked = true;
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.lock = true; }
  require_api(sdrplay_api_GetDevices(devs, &ndev, sizeof(devs) / sizeof(sdrplay_api_DeviceT)), "Enumerate receivers");
  if (ndev > sizeof(devs) / sizeof(sdrplay_api_DeviceT))
    throw std::runtime_error("[RspDuo] API returned an invalid receiver count.");
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.enumerate = true; }

  std::cerr << "[RspDuo] MaxDevs=" << sizeof(devs) / 
    sizeof(sdrplay_api_DeviceT) << " NumDevs=" << ndev << std::endl;

  if (ndev == 0)
  {
    std::cerr << "Error: No devices found" << std::endl;
    throw std::runtime_error("[RspDuo] No receiver found. Check USB connection and permissions.");
  }

  // Select exactly the requested unit. A family choice does not authorize
  // silently choosing the first physical receiver when several are present.
  unsigned int matching = 0;
  for (i = 0; i < ndev; i++)
  {
    if (devs[i].hwVer == SDRPLAY_RSPduo_ID &&
        (requestedSerial.empty() || requestedSerial == sdk_serial(devs[i])))
    {
      chosenIdx = i;
      ++matching;
    }
  }

  if (matching == 0)
  {
    throw std::runtime_error("[RspDuo] No matching RSPduo found; check the selected serial and USB connection.");
  }
  if (matching != 1)
    throw std::runtime_error("[RspDuo] Multiple RSPduos found; select one serial in Receiver settings before starting.");

  chosenDevice = &devs[chosenIdx];
  const auto selectedSerial = sdk_serial(*chosenDevice);
  chosenDevice->tuner = sdrplay_api_Tuner_Both;
  chosenDevice->rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;

  std::cerr << "[RspDuo] Device ID " << chosenIdx << std::endl;
  std::cerr << "[RspDuo] Serial Number " << chosenDevice->SerNo << std::endl;
  std::cerr << "[RspDuo] Hardware Version " << std::to_string(chosenDevice->hwVer) << std::endl;
  std::cerr << "[RspDuo] Tuner " << std::hex << chosenDevice->tuner << std::dec << std::endl;
  std::cerr << "[RspDuo] RspDuoMode " << std::hex << chosenDevice->rspDuoMode << std::dec << std::endl;

  require_api(sdrplay_api_SelectDevice(chosenDevice), "Select RSPduo in dual-tuner mode");
  deviceSelected = true;
  {
    std::lock_guard<std::mutex> lock(receiptMutex);
    startupStages.select = true;
    startupStages.selectedSerial = selectedSerial;
    startupStages.deviceIndex = chosenIdx;
    startupStages.hardwareVersion = chosenDevice->hwVer;
  }
  require_api(sdrplay_api_UnlockDeviceApi(), "Unlock device API");
  apiLocked = false;
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.unlock = true; }
  // Keep SDK errors, but avoid emitting thousands of
  // verbose service/journal lines during an RF overload/AGC burst. A caller
  // may opt back into the exact original behavior for comparison.
  const char* debugLevel = std::getenv("VECTORWARP_SDRPLAY_DEBUG_LEVEL");
  if (debugLevel && std::strcmp(debugLevel, "verbose") != 0 &&
      std::strcmp(debugLevel, "error") != 0)
    throw std::invalid_argument("VECTORWARP_SDRPLAY_DEBUG_LEVEL must be error or verbose");
  require_api(sdrplay_api_DebugEnable(chosenDevice->dev,
    debugLevel && std::strcmp(debugLevel, "verbose") == 0 ?
      sdrplay_api_DbgLvl_Verbose : sdrplay_api_DbgLvl_Error),
    "Enable API diagnostic logging");
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.debugEnable = true; }

  return;
}

void RspDuo::set_device_parameters()
{
  // retrieve device parameters so they can be changed if wanted
  require_api(sdrplay_api_GetDeviceParams(chosenDevice->dev, &deviceParams), "Read dual-tuner parameters");

  // check for NULL pointer before changing settings
  if (deviceParams == NULL)
  {
    std::cout << "Error: Device parameters pointer is null" << std::endl;
    throw std::runtime_error("[RspDuo] API omitted device or dual-tuner parameters.");
  }
  // Check devParams separately before reading fsFreq for the guarded scaled
  // counter mode below.
  if (deviceParams->devParams == NULL || deviceParams->rxChannelA == NULL ||
      deviceParams->rxChannelB == NULL)
    throw std::runtime_error("[RspDuo] API omitted device or dual-tuner parameters.");
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.getDeviceParams = true; }

  if (scaledSampleCounter && !SdkSampleClock::supportsRatio3({
      deviceParams->devParams->fsFreq.fsHz, fs, static_cast<unsigned>(nDecimation),
      ifType == sdrplay_api_IF_1_620,
      chosenDevice->rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner}))
    throw std::runtime_error("[RspDuo] Scaled SDK counter is validated only for dual-tuner 6MHz ADC / 2MSps output.");

  // set USB mode
  if (usb_bulk_fg)
  {
    deviceParams->devParams->mode = sdrplay_api_BULK;
  }
  else
  {
    deviceParams->devParams->mode = sdrplay_api_ISOCH;
  }

  // rxChannelA and rxChannelB are separate API parameter records. Set every
  // required shared RF/AGC/IF/decimation/notch field on each, preserving their
  // separate gains and tuner-specific defaults. These are requested values;
  // successful Init/Update still does not prove physical RF/coherence.
  deviceParams->rxChannelA->tunerParams.gain.gRdB = gain_reduction_nr_a;
  deviceParams->rxChannelA->tunerParams.gain.LNAstate = lna_state_nr;
  deviceParams->rxChannelB->tunerParams.gain.gRdB = gain_reduction_nr_b;
  deviceParams->rxChannelB->tunerParams.gain.LNAstate = lna_state_nr;
  for (auto* channel : {deviceParams->rxChannelA, deviceParams->rxChannelB}) {
    channel->tunerParams.rfFreq.rfHz = fc;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    if (agc_bandwidth_nr == 5) channel->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
    else if (agc_bandwidth_nr == 50) channel->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    else if (agc_bandwidth_nr == 100) channel->ctrlParams.agc.enable = sdrplay_api_AGC_100HZ;
    if (channel->ctrlParams.agc.enable != sdrplay_api_AGC_DISABLE)
      channel->ctrlParams.agc.setPoint_dBfs = agc_set_point_nr;
    channel->ctrlParams.decimation.enable = 1;
    channel->ctrlParams.decimation.decimationFactor = nDecimation;
    channel->tunerParams.ifType = ifType;
    channel->tunerParams.bwType = bwType;
    channel->rspDuoTunerParams.rfNotchEnable = rf_notch_fg;
    channel->rspDuoTunerParams.rfDabNotchEnable = dab_notch_fg;
  }

  // assign callback functions to be passed to sdrplay_api_Init()
  cbFns.StreamACbFn = _stream_a_callback;
  cbFns.StreamBCbFn = _stream_b_callback;
  cbFns.EventCbFn = _event_callback;

  return;
}

void RspDuo::stream_a_callback(short *xi, short *xq, 
sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, 
unsigned int reset, void *cbContext)
{
  std::lock_guard<std::mutex> lock(callbackMutex);
  if (!run_fg || callbackFault.load(std::memory_order_relaxed)) return;
  const bool malformed = !xi || !xq || !params || !numSamples || numSamples > MAX_CALLBACK_SAMPLES;
  const bool scaledRateChanged = !malformed && scaledSampleCounter && params->fsChanged;
  bool discontinuity = false;
  if (!malformed && !reset) {
    if (scaledSampleCounter) {
      const auto observed = sampleClock.observe(params->firstSampleNum, numSamples);
      discontinuity = observed.sequenceBreak;
    } else {
      discontinuity = expectedFirstSampleValid &&
        !rspduo_sequence::continues(expectedFirstSample, params->firstSampleNum);
    }
  }
  if (reset || malformed || discontinuity || scaledRateChanged) {
    clear_pending_locked();
    expectedFirstSampleValid = false;
    sampleClock.clear();
    if ((malformed && !reset) || scaledRateChanged || streamEstablished.load(std::memory_order_relaxed))
      signal_callback_fault(malformed ? "stream A invalid callback" :
        scaledRateChanged ? "stream A sample rate changed" :
        discontinuity ? "stream A sample counter gap" : "stream A reset");
    recording_discontinuity("RSPduo stream A reset, sample-rate change, invalid sample count, or sample sequence discontinuity");
    return;
  }
  unsigned int i = 0;
  unsigned int j = 0;

  int freeSlot = -1;
  for (int slot = 0; slot < static_cast<int>(callbackStorage.size()); ++slot)
    if (!callbackSlotBusy[slot]) { freeSlot = slot; break; }
  if (freeSlot < 0) {
    expectedFirstSampleValid = false;
    sampleClock.clear();
    signal_callback_fault("stream A callback storage exhausted while B publishes");
    recording_discontinuity("RSPduo callback storage exhausted before stream A");
    return;
  }
  short* paired = callbackStorage[freeSlot].data();
  callbackSlotBusy[freeSlot] = true;
  pendingSamples[freeSlot] = numSamples;
  pendingFirstSamples[freeSlot] = params->firstSampleNum;
  pendingSlots[(pendingHead + pendingCount) % CALLBACK_SLOTS] = freeSlot;
  ++pendingCount;
  expectedFirstSample = rspduo_sequence::next(params->firstSampleNum, numSamples);
  expectedFirstSampleValid = true;

  // IIQQxxxx
  for (i = 0; i < numSamples; i++)
  {
    // add tuner A data
    paired[j++] = xi[i];
    paired[j++] = xq[i];
    // skip tuner B data
    j++;
    j++;
  }

  // find max for stats
  if (stats_fg)
  {
    for (i = 0; i < numSamples; i++)
    {
      if (xi[i] > max_a_nr)
      {
        max_a_nr = xi[i];
      }
    }
  }

  return;
}

void RspDuo::stream_b_callback(short *xi, short *xq, 
sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, 
unsigned int reset, void *cbContext)
{
  // Take this before callbackMutex.  A callbacks only take callbackMutex, so
  // they can populate the second slot while an earlier B callback publishes.
  // A later B cannot commit ahead of this callback because std::mutex does
  // not provide FIFO fairness by itself.
  std::lock_guard<std::mutex> processingLock(callbackBProcessingMutex);
  short* paired = nullptr;
  int pairedSlot = -1;
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    if (!run_fg || callbackFault.load(std::memory_order_relaxed)) return;
    const bool malformed = !xi || !xq || !params || !numSamples || numSamples > MAX_CALLBACK_SAMPLES;
    const bool scaledRateChanged = !malformed && scaledSampleCounter && params->fsChanged;
    const int pendingSlot = pendingCount ? int(pendingSlots[pendingHead]) : -1;
    const bool pairingMismatch = !malformed && (pendingSlot < 0 ||
      pendingSamples[pendingSlot] != numSamples ||
      pendingFirstSamples[pendingSlot] != params->firstSampleNum);
    if (reset || malformed || pairingMismatch || scaledRateChanged) {
      clear_pending_locked();
      expectedFirstSampleValid = false;
      sampleClock.clear();
      if ((malformed && !reset) || scaledRateChanged || streamEstablished.load(std::memory_order_relaxed))
        signal_callback_fault(malformed ? "stream B invalid callback" :
          scaledRateChanged ? "stream B sample rate changed" :
          pairingMismatch ? "stream B did not match pending A" : "stream B reset");
      recording_discontinuity("RSPduo callback pairing reset, sample-rate change, gap, sample-count, or epoch mismatch");
      return;
    }
    pairedSlot = pendingSlot;
    paired = callbackStorage[pairedSlot].data();
    pendingHead = (pendingHead + 1) % CALLBACK_SLOTS;
    --pendingCount;
    streamEstablished.store(true, std::memory_order_relaxed);
  }
  try {
  unsigned int i = 0;
  unsigned int j = 0;

  // xxxxIIQQ
  for (i = 0; i < numSamples; i++)
  {
    // skip tuner A data
    j++;
    j++;
    // add tuner B data
    paired[j++] = xi[i];
    paired[j++] = xq[i];
  }

  if (!pairedCpiQueue) {
    // Fixed order avoids inter-channel deadlock; RAII releases both locks if
    // a FIFO allocation throws and the C callback wrapper converts it to a
    // fault. Keep them only for the FIFO append.
    std::unique_lock<IqData> outputLock1(*outputBuffer1);
    std::unique_lock<IqData> outputLock2(*outputBuffer2);
    for (i = 0; i < numSamples*4; i+=4)
    {
      outputBuffer1->push_back({(double)paired[i], (double)paired[i+1]});
      outputBuffer2->push_back({(double)paired[i+2], (double)paired[i+3]});
    }
  }

  if (pairedCpiQueue) {
    // The validated SDK counter can change phase at an ADC rollover. The A
    // callback already checks continuity; use delivered-sample positions for
    // the queue's contiguous CPI assembly.
    pairedCpiQueue->push(reinterpret_cast<const int16_t*>(paired), numSamples,
      pairedPublishedSamples);
    pairedPublishedSamples += numSamples;
  }

  // write data to file
  if (is_recording() && numSamples) {
    blah2::IqBlock block(2, std::vector<std::complex<float>>(numSamples));
    for (unsigned sample=0; sample<numSamples; ++sample)
      for (unsigned ch=0; ch<2; ++ch)
        block[ch][sample] = {float(paired[sample*4+ch*2]), float(paired[sample*4+ch*2+1])};
    record_block(block);
  }

  // find max for stats
  if (stats_fg)
  {
    for (i = 0; i < numSamples; i++)
    {
      if (xi[i] > max_b_nr)
      {
        max_b_nr = xi[i];
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(callbackMutex);
    callbackSlotBusy[pairedSlot] = false;
  }
  } catch (...) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    callbackSlotBusy[pairedSlot] = false;
    throw;
  }
  return;
}

void RspDuo::event_callback(sdrplay_api_EventT eventId,
  sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params,
  void *cbContext)
{
  (void)params;
  (void)cbContext;
  const bool tunerA = tuner == sdrplay_api_Tuner_A;
  switch (eventId) {
  case sdrplay_api_GainChange:
    (tunerA ? gainEventsA : gainEventsB).fetch_add(1, std::memory_order_relaxed);
    break;
  case sdrplay_api_PowerOverloadChange: {
    (tunerA ? overloadEventsA : overloadEventsB).fetch_add(1, std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    const auto beginNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      begin.time_since_epoch()).count();
    ackTuner.store(tunerA ? 0 : 1, std::memory_order_relaxed);
    ackStartNs.store(beginNs, std::memory_order_release);
    // Preserve the vendor example's synchronous overload acknowledgement.
    const auto status = sdrplay_api_Update(chosenDevice->dev, tuner,
      sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
    ackStartNs.store(0, std::memory_order_release);
    ackLastStatus.store(static_cast<int>(status), std::memory_order_relaxed);
    ackCount.fetch_add(1, std::memory_order_relaxed);
    if (status != sdrplay_api_Success)
      ackErrorCount.fetch_add(1, std::memory_order_relaxed);
    const auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - begin).count();
    auto previous = ackMaxUs.load(std::memory_order_relaxed);
    while (previous < static_cast<uint64_t>(durationUs) &&
      !ackMaxUs.compare_exchange_weak(previous, durationUs, std::memory_order_relaxed)) {}
    break;
  }
  case sdrplay_api_DeviceRemoved:
    std::cerr << "[RspDuo] Device removed" << std::endl;
    deviceRemoved = true;
    run_fg = false;
    break;
  default:
    std::cerr << "[RspDuo] Unknown event " << eventId << std::endl;
    break;
  }
}

void RspDuo::initialise_device()
{
  require_api(sdrplay_api_Init(chosenDevice->dev, &cbFns, this), "Initialize dual-tuner streaming");
  deviceInitialized = true;
  { std::lock_guard<std::mutex> lock(receiptMutex); startupStages.init = true; }
}

void RspDuo::uninitialise_device()
{
  cleanup_api();
}

void RspDuo::cleanup_api() noexcept
{
  auto report = [](sdrplay_api_ErrT result, const char* operation) {
    if (result != sdrplay_api_Success)
      std::cerr << "[RspDuo] Cleanup " << operation << ": " << sdrplay_api_GetErrorString(result) << '\n';
  };
  if (deviceInitialized) { report(sdrplay_api_Uninit(chosenDevice->dev), "uninitialize"); deviceInitialized = false; }
  if (deviceSelected) { report(sdrplay_api_ReleaseDevice(chosenDevice), "release"); deviceSelected = false; }
  if (apiLocked) { report(sdrplay_api_UnlockDeviceApi(), "unlock"); apiLocked = false; }
  if (apiOpened) { report(sdrplay_api_Close(), "close"); apiOpened = false; }
  chosenDevice = nullptr;
  deviceParams = nullptr;
}

std::string RspDuo::startup_receipt_json() const
{
  StartupStages stages;
  {
    std::lock_guard<std::mutex> lock(receiptMutex);
    stages = startupStages;
  }
  // The API must still receive the base error status for malformed direct
  // YAML. Such a request cannot satisfy the receipt contract.
  if (!stages.settingsValidated) return {};
  const bool accepted = stages.open && stages.apiVersion && stages.lock &&
    stages.enumerate && stages.select && stages.unlock && stages.debugEnable &&
    stages.getDeviceParams && stages.init && stages.gainUpdateA && stages.gainUpdateB;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::boolalpha << std::setprecision(std::numeric_limits<float>::max_digits10);
  out << "{\"schema\":1,\"receiver\":\"RspDuo\",\"status\":\""
      << (accepted ? "accepted" : "pending")
      << "\",\"hardwareVerified\":false,\"readbackAvailable\":false,\"requested\":{"
      << "\"serial\":" << json_string(requestedSerial)
      << ",\"frequency\":" << fc << ",\"sampleRate\":" << fs
      << ",\"agcSetPoint\":" << agc_set_point_nr
      << ",\"bandwidthNumber\":" << agc_bandwidth_nr
      << ",\"gainReduction\":[" << gain_reduction_nr_a << ',' << gain_reduction_nr_b << ']'
      << ",\"lnaState\":" << lna_state_nr
      << ",\"dabNotch\":" << dab_notch_fg << ",\"rfNotch\":" << rf_notch_fg
      << ",\"ifBandwidthKhz\":" << static_cast<int>(bwType)
      << ",\"ifFrequencyKhz\":" << static_cast<int>(ifType)
      << ",\"decimation\":" << nDecimation << "},\"selected\":";
  if (stages.select)
    out << "{\"serial\":" << json_string(stages.selectedSerial)
        << ",\"deviceIndex\":" << stages.deviceIndex
        << ",\"hardwareVersion\":" << stages.hardwareVersion
        << ",\"tuner\":\"Both\",\"mode\":\"Dual_Tuner\"}";
  else out << "null";
  out << ",\"sdk\":{\"version\":";
  if (stages.sdkVersion > 0) out << stages.sdkVersion;
  else out << "null";
  out << ",\"stages\":{\"open\":" << stages.open
      << ",\"apiVersion\":" << stages.apiVersion
      << ",\"lock\":" << stages.lock
      << ",\"enumerate\":" << stages.enumerate
      << ",\"select\":" << stages.select
      << ",\"unlock\":" << stages.unlock
      << ",\"debugEnable\":" << stages.debugEnable
      << ",\"getDeviceParams\":" << stages.getDeviceParams
      << ",\"init\":" << stages.init
      << ",\"gainUpdateA\":" << stages.gainUpdateA
      << ",\"gainUpdateB\":" << stages.gainUpdateB << "}}}";
  return out.str();
}
