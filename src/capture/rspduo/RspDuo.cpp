#include "RspDuo.h"
#include "SampleSequence.h"

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

static void require_api(sdrplay_api_ErrT result, const char* operation)
{
  if (result != sdrplay_api_Success)
    throw std::runtime_error(std::string("[RspDuo] ") + operation + ": " + sdrplay_api_GetErrorString(result));
}

// class static constants
const double RspDuo::MAX_FREQUENCY_NR = 2000000000;
const int RspDuo::MIN_AGC_SET_POINT_NR = -72;        // min agc set point
const int RspDuo::MIN_GAIN_REDUCTION_NR = 20;        // min gain reduction
const int RspDuo::MAX_GAIN_REDUCTION_NR = 59;        // max gain reduction
const int RspDuo::MAX_LNA_STATE_NR = 9;              // max lna state
const int RspDuo::DEF_SAMPLE_RATE_NR = 2000000;      // default sample rate

// global variables (SDRPlay)
sdrplay_api_DeviceT *chosenDevice = NULL;
sdrplay_api_DeviceT devs[1023];
sdrplay_api_DeviceParamsT *deviceParams = NULL;
sdrplay_api_ErrT err;
sdrplay_api_CallbackFnsT cbFns;

// global variables
short *buffer_16_ar = NULL;
unsigned int buffer_16_samples = 0;
unsigned int buffer_16_first_sample = 0;
uint32_t expected_first_sample = 0;
bool expected_first_sample_valid = false;
std::mutex buffer_16_mutex;
constexpr unsigned int MAX_CALLBACK_SAMPLES = 262144;

short max_a_nr = 0;
short max_b_nr = 0;
std::atomic<bool> run_fg{true};
bool stats_fg = true;
Source* recordingSource = nullptr;
IqData *buffer1;
IqData *buffer2;

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
  usb_bulk_fg = false;
  recordingSource = this;
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
  validate();
  run_fg = true;
  deviceRemoved = false;
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
  buffer1 = _buffer1;
  buffer2 = _buffer2;

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
  require_api(sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_B,
    sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None), "Update tuner B gain");
  lock.unlock();

  // control loop
  while (run_fg)
  {
    if (stats_fg)
    {
      std::cerr << "[RspDuo]" << " max_a_nr: " << max_a_nr << 
        " max_b_nr: " << max_b_nr << std::endl;
      max_a_nr = 0;
      max_b_nr = 0;
    }
    sleep(1);
  }
  if (deviceRemoved) throw std::runtime_error("[RspDuo] Receiver disconnected. Reconnect it and check the SDRplay API service before restarting.");
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
    if (lna_state_nr < 1 || lna_state_nr > MAX_LNA_STATE_NR) {
        std::cerr << "Error: LNA state must be between 1 and " << MAX_LNA_STATE_NR << std::endl;
        throw std::invalid_argument("[RspDuo] LNA state must be between 1 and 9.");
    }

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
  require_api(sdrplay_api_ApiVersion(&ver), "Read API version");
  if (!std::isfinite(ver) || std::abs(ver - SDRPLAY_API_VERSION) > 0.001f)
    throw std::runtime_error("[RspDuo] Installed SDRplay API version does not match this receiver build.");
}

void RspDuo::get_device()
{
  unsigned int i;
  unsigned int ndev;
  unsigned int chosenIdx = 0;

  require_api(sdrplay_api_LockDeviceApi(), "Lock API for device selection");
  apiLocked = true;
  require_api(sdrplay_api_GetDevices(devs, &ndev, sizeof(devs) / sizeof(sdrplay_api_DeviceT)), "Enumerate receivers");
  if (ndev > sizeof(devs) / sizeof(sdrplay_api_DeviceT))
    throw std::runtime_error("[RspDuo] API returned an invalid receiver count.");

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
        (requestedSerial.empty() || requestedSerial == devs[i].SerNo))
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
  chosenDevice->tuner = sdrplay_api_Tuner_Both;
  chosenDevice->rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;

  std::cerr << "[RspDuo] Device ID " << chosenIdx << std::endl;
  std::cerr << "[RspDuo] Serial Number " << chosenDevice->SerNo << std::endl;
  std::cerr << "[RspDuo] Hardware Version " << std::to_string(chosenDevice->hwVer) << std::endl;
  std::cerr << "[RspDuo] Tuner " << std::hex << chosenDevice->tuner << std::dec << std::endl;
  std::cerr << "[RspDuo] RspDuoMode " << std::hex << chosenDevice->rspDuoMode << std::dec << std::endl;

  require_api(sdrplay_api_SelectDevice(chosenDevice), "Select RSPduo in dual-tuner mode");
  deviceSelected = true;
  require_api(sdrplay_api_UnlockDeviceApi(), "Unlock device API");
  apiLocked = false;
  require_api(sdrplay_api_DebugEnable(chosenDevice->dev, sdrplay_api_DbgLvl_Verbose), "Enable API diagnostic logging");

  return;
}

void RspDuo::set_device_parameters()
{
  // retrieve device parameters so they can be changed if wanted
  require_api(sdrplay_api_GetDeviceParams(chosenDevice->dev, &deviceParams), "Read dual-tuner parameters");

  // check for NULL pointer before changing settings
  if (deviceParams == NULL || deviceParams->devParams == NULL ||
      deviceParams->rxChannelA == NULL || deviceParams->rxChannelB == NULL)
  {
    std::cout << "Error: Device parameters pointer is null" << std::endl;
    throw std::runtime_error("[RspDuo] API omitted device or dual-tuner parameters.");
  }

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
  std::lock_guard<std::mutex> lock(buffer_16_mutex);
  if (reset || !xi || !xq || !params || !numSamples || numSamples > MAX_CALLBACK_SAMPLES ||
      (expected_first_sample_valid && !rspduo_sequence::continues(expected_first_sample, params->firstSampleNum))) {
    if (buffer_16_ar) { free(buffer_16_ar); buffer_16_ar = NULL; buffer_16_samples = 0; buffer_16_first_sample = 0; }
    expected_first_sample_valid = false;
    if (recordingSource) recordingSource->recording_discontinuity(
      "RSPduo stream A reset, invalid sample count, or sample sequence discontinuity");
    return;
  }
  if (buffer_16_ar) {
    free(buffer_16_ar); buffer_16_ar = NULL; buffer_16_samples = 0; buffer_16_first_sample = 0;
    expected_first_sample_valid = false;
    if (recordingSource) recordingSource->recording_discontinuity("RSPduo callback pairing gap before stream A");
    return;
  }
  unsigned int i = 0;
  unsigned int j = 0;

  // process stream callback data
  buffer_16_ar = (short int *)malloc(numSamples * 4 * sizeof(short));

  if (buffer_16_ar == NULL)
  {
    std::cout << "Error: stream_a_callback, malloc failed" << std::endl;
    if (recordingSource) recordingSource->recording_discontinuity("RSPduo stream A allocation failed");
    run_fg = false;
    return;
  }
  buffer_16_samples = numSamples;
  buffer_16_first_sample = params->firstSampleNum;

  // IIQQxxxx
  for (i = 0; i < numSamples; i++)
  {
    // add tuner A data
    buffer_16_ar[j++] = xi[i];
    buffer_16_ar[j++] = xq[i];
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
  short* paired = NULL;
  {
    std::lock_guard<std::mutex> lock(buffer_16_mutex);
    if (reset || !xi || !xq || !params || !numSamples || numSamples > MAX_CALLBACK_SAMPLES ||
        !buffer_16_ar || buffer_16_samples != numSamples ||
        buffer_16_first_sample != params->firstSampleNum) {
      if (buffer_16_ar) free(buffer_16_ar);
      buffer_16_ar = NULL; buffer_16_samples = 0; buffer_16_first_sample = 0;
      expected_first_sample_valid = false;
      if (recordingSource) recordingSource->recording_discontinuity(
        "RSPduo callback pairing reset, gap, sample-count, or epoch mismatch");
      return;
    }
    paired = buffer_16_ar;
    buffer_16_ar = NULL; buffer_16_samples = 0; buffer_16_first_sample = 0;
    expected_first_sample = rspduo_sequence::next(params->firstSampleNum, numSamples);
    expected_first_sample_valid = true;
  }
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

  // write data to IqData
  buffer1->lock();
  buffer2->lock();
  for (i = 0; i < numSamples*4; i+=4)
  {
    buffer1->push_back({(double)paired[i], (double)paired[i+1]});
    buffer2->push_back({(double)paired[i+2], (double)paired[i+3]});
  }
  buffer1->unlock();
  buffer2->unlock();

  // write data to file
  if (recordingSource && recordingSource->is_recording() && numSamples) {
    blah2::IqBlock block(2, std::vector<std::complex<float>>(numSamples));
    for (unsigned sample=0; sample<numSamples; ++sample)
      for (unsigned ch=0; ch<2; ++ch)
        block[ch][sample] = {float(paired[sample*4+ch*2]), float(paired[sample*4+ch*2+1])};
    recordingSource->record_block(block);
  }

  free(paired);

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

  return;
}

void RspDuo::event_callback(sdrplay_api_EventT eventId, 
sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, 
void *cbContext)
{
  std::string tuner_str = (tuner == sdrplay_api_Tuner_A) ? 
    "sdrplay_api_Tuner_A" : "sdrplay_api_Tuner_B";
  switch (eventId)
  {
  case sdrplay_api_GainChange:
    std::cerr << "[RspDuo] Gain change, tuner=" << tuner_str << " ";
    std::cerr << "gRdB=" << params->gainParams.gRdB << " ";
    std::cerr << "lnaGRdB=" << params->gainParams.lnaGRdB << " ";
    std::cerr << "systemGain=" << params->gainParams.currGain << std::endl;
    break;

  case sdrplay_api_PowerOverloadChange:
    std::cerr << "[RspDuo] PowerOverloadChange, tuner=" << tuner_str << " ";
    std::cerr << "powerOverloadChangeType=" << 
      ((params->powerOverloadParams.powerOverloadChangeType 
      == sdrplay_api_Overload_Detected) ? "sdrplay_api_Overload_Detected" : 
      "sdrplay_api_Overload_Corrected") << std::endl;
    // send update message to acknowledge power overload message received
    sdrplay_api_Update(chosenDevice->dev, tuner, 
      sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
    break;

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
