// Link with RspDuo.cpp, Source.cpp and Recording.cpp; never link SDRplay's
// runtime library. These SDK functions are local failure-injection stubs.
#include "capture/rspduo/RspDuo.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
extern std::atomic<bool> run_fg;

namespace {
std::string failAt;
unsigned opened, closed, locked, unlocked, selected, released, initialized, uninitialized;
sdrplay_api_DevParamsT deviceParameters{};
sdrplay_api_RxChannelParamsT tunerA{}, tunerB{};
sdrplay_api_DeviceParamsT parameters{&deviceParameters, &tunerA, &tunerB};
bool nullParameters = false, removeOnInit = false, multipleDevices = false, stopAfterGain = false;
std::string selectedSerial;
void* callbackContext = nullptr;
sdrplay_api_ErrT result(const char* stage) { return failAt == stage ? sdrplay_api_Fail : sdrplay_api_Success; }
void reset() {
  opened = closed = locked = unlocked = selected = released = initialized = uninitialized = 0;
  deviceParameters = {}; tunerA = {}; tunerB = {};
  parameters = {&deviceParameters, &tunerA, &tunerB};
  failAt.clear(); nullParameters = removeOnInit = multipleDevices = stopAfterGain = false;
  selectedSerial.clear(); callbackContext = nullptr;
}
}

sdrplay_api_ErrT sdrplay_api_Open() { if (result("open") == sdrplay_api_Success) ++opened; return result("open"); }
sdrplay_api_ErrT sdrplay_api_Close() { ++closed; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_ApiVersion(float* version) { *version = failAt == "version-mismatch" ? 1.0f : SDRPLAY_API_VERSION; return result("version"); }
sdrplay_api_ErrT sdrplay_api_LockDeviceApi() { if (result("lock") == sdrplay_api_Success) ++locked; return result("lock"); }
sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi() { ++unlocked; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT* devices, unsigned* count, unsigned) {
  *count = failAt == "none" ? 0 : (multipleDevices ? 2 : 1);
  devices[0] = {}; devices[0].hwVer = failAt == "wrong-model" ? SDRPLAY_RSP1_ID : SDRPLAY_RSPduo_ID;
  std::strcpy(devices[0].SerNo, "mock-only");
  if (multipleDevices) { devices[1] = devices[0]; std::strcpy(devices[1].SerNo, "mock-second"); }
  return result("enumerate");
}
sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT* device) {
  assert(device->tuner == sdrplay_api_Tuner_Both);
  assert(device->rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner);
  selectedSerial = device->SerNo;
  if (result("select") == sdrplay_api_Success) ++selected;
  return result("select");
}
sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT*) { ++released; return sdrplay_api_Success; }
const char* sdrplay_api_GetErrorString(sdrplay_api_ErrT) { return "injected failure"; }
sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE, sdrplay_api_DbgLvl_t) { return result("debug"); }
sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE, sdrplay_api_DeviceParamsT** output) {
  *output = nullParameters ? nullptr : &parameters; return result("parameters");
}
sdrplay_api_ErrT sdrplay_api_Init(HANDLE, sdrplay_api_CallbackFnsT* callbacks, void* context) {
  assert(context); callbackContext = context;
  if (result("init") == sdrplay_api_Success) ++initialized;
  if (removeOnInit) { sdrplay_api_EventParamsT event{}; callbacks->EventCbFn(sdrplay_api_DeviceRemoved, sdrplay_api_Tuner_Both, &event, context); }
  return result("init");
}
sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE) { ++uninitialized; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_Update(HANDLE, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T) {
  if (stopAfterGain && tuner == sdrplay_api_Tuner_B) run_fg = false;
  return result(tuner == sdrplay_api_Tuner_A ? "gain-a" : "gain-b");
}
// No IQ buffers are used by this fixture; these satisfy the linked callback
// methods, and must never execute during an injected startup failure.
void IqData::lock() { assert(false); }
void IqData::unlock() { assert(false); }
void IqData::push_back(std::complex<double>) { assert(false); }

int main() {
  bool record = false;
  for (uint32_t rate : {2000000u, 1000000u, 500000u, 250000u, 125000u, 62500u}) {
    for (int agc : {0, 5, 50, 100}) {
      reset(); multipleDevices = true; stopAfterGain = true;
      RspDuo receiver("RspDuo", 527000000, rate, "/unused", &record,
        -37, agc, 25, 47, 5, agc != 0, agc == 0, "mock-second");
      receiver.start();
      assert(selectedSerial == "mock-second");
      for (auto* tuner : {&tunerA, &tunerB}) {
        assert(tuner->tunerParams.rfFreq.rfHz == 527000000);
        assert(tuner->tunerParams.gain.LNAstate == 5);
        assert(tuner->tunerParams.ifType == sdrplay_api_IF_1_620);
        assert(tuner->ctrlParams.decimation.enable == 1);
        assert(tuner->ctrlParams.decimation.decimationFactor == 2000000 / rate);
        const auto expectedAgc = agc == 0 ? sdrplay_api_AGC_DISABLE :
          (agc == 5 ? sdrplay_api_AGC_5HZ : (agc == 50 ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_100HZ));
        assert(tuner->ctrlParams.agc.enable == expectedAgc);
        if (agc) assert(tuner->ctrlParams.agc.setPoint_dBfs == -37);
        assert(tuner->rspDuoTunerParams.rfNotchEnable == (agc == 0));
        assert(tuner->rspDuoTunerParams.rfDabNotchEnable == (agc != 0));
      }
      assert(tunerA.tunerParams.gain.gRdB == 25 && tunerB.tunerParams.gain.gRdB == 47);
      receiver.process(nullptr, nullptr); // Fake SDK stops the test loop after both accepted Updates.
      assert(initialized == 1 && callbackContext == &receiver);
      receiver.stop(); receiver.stop();
      assert(opened == closed && locked == unlocked && selected == released && initialized == uninitialized);
    }
  }
  for (const std::string serial : {"", "not-present"}) {
    reset(); multipleDevices = true;
    RspDuo receiver("RspDuo", 527000000, 2000000, "/unused", &record,
      -30, 50, 30, 31, 3, true, true, serial);
    bool failed = false;
    try { receiver.start(); } catch (const std::exception&) { failed = true; }
    assert(failed && selected == 0 && opened == closed && locked == unlocked);
  }
  reset();
  RspDuo tooFast("RspDuo", 527000000, 6000000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
  try { tooFast.start(); assert(false); } catch (const std::exception&) {}
  assert(opened == 0); // Six-MS/s output is not offered by this dual-tuner adapter.
  for (const auto& stage : {"open", "version", "version-mismatch", "lock", "enumerate", "none", "wrong-model", "select", "debug", "parameters"}) {
    reset(); failAt = stage;
    RspDuo receiver("RspDuo", 204640000, 2000000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
    bool failed = false;
    try { receiver.start(); } catch (const std::exception& error) { failed = true; assert(std::string(error.what()).find("[RspDuo]") != std::string::npos); }
    assert(failed); receiver.stop(); receiver.stop();
    assert(opened == closed && locked == unlocked && selected == released);
    assert(initialized == 0 && uninitialized == 0);
  }
  reset(); nullParameters = true;
  RspDuo missing("RspDuo", 204640000, 2000000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
  try { missing.start(); assert(false); } catch (const std::exception&) {}
  assert(opened == closed && selected == released);
  for (const auto& stage : {"init", "gain-a", "gain-b", "removed"}) {
    reset(); failAt = stage; removeOnInit = failAt == "removed";
    RspDuo receiver("RspDuo", 204640000, 500000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
    receiver.start();
    assert(tunerA.tunerParams.rfFreq.rfHz == 204640000 && tunerB.tunerParams.rfFreq.rfHz == 204640000);
    assert(tunerA.ctrlParams.decimation.decimationFactor == 4 && tunerB.ctrlParams.decimation.decimationFactor == 4);
    assert(tunerA.ctrlParams.agc.setPoint_dBfs == -30 && tunerB.ctrlParams.agc.setPoint_dBfs == -30);
    assert(tunerA.rspDuoTunerParams.rfNotchEnable && tunerB.rspDuoTunerParams.rfNotchEnable);
    assert(tunerA.rspDuoTunerParams.rfDabNotchEnable && tunerB.rspDuoTunerParams.rfDabNotchEnable);
    assert(tunerA.tunerParams.gain.gRdB == 30 && tunerB.tunerParams.gain.gRdB == 31);
    bool failed = false;
    try { receiver.process(nullptr, nullptr); } catch (const std::exception&) { failed = true; }
    assert(failed && callbackContext == &receiver);
    receiver.stop(); receiver.stop();
    assert(opened == closed && locked == unlocked && selected == released && initialized == uninitialized);
  }
  std::cout << "RSPduo mocked API: 24 complete dual-tuner setting tuples, exact serial/multiple-device refusal, unsupported6MS/s refusal, 15 failure stages and idempotent cleanup passed. No hardware opened.\n";
}
