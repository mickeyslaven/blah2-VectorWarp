// Link with RspDuo.cpp, Source.cpp and Recording.cpp; never link SDRplay's
// runtime library. These SDK functions are local failure-injection stubs.
#include "capture/rspduo/RspDuo.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
std::string failAt;
unsigned opened, closed, locked, unlocked, selected, released, initialized, uninitialized;
sdrplay_api_DevParamsT deviceParameters{};
sdrplay_api_RxChannelParamsT tunerA{}, tunerB{};
sdrplay_api_DeviceParamsT parameters{&deviceParameters, &tunerA, &tunerB};
bool nullParameters = false, removeOnInit = false;
void* callbackContext = nullptr;
sdrplay_api_ErrT result(const char* stage) { return failAt == stage ? sdrplay_api_Fail : sdrplay_api_Success; }
void reset() {
  opened = closed = locked = unlocked = selected = released = initialized = uninitialized = 0;
  deviceParameters = {}; tunerA = {}; tunerB = {};
  parameters = {&deviceParameters, &tunerA, &tunerB};
  failAt.clear(); nullParameters = removeOnInit = false; callbackContext = nullptr;
}
}

sdrplay_api_ErrT sdrplay_api_Open() { if (result("open") == sdrplay_api_Success) ++opened; return result("open"); }
sdrplay_api_ErrT sdrplay_api_Close() { ++closed; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_ApiVersion(float* version) { *version = failAt == "version-mismatch" ? 1.0f : SDRPLAY_API_VERSION; return result("version"); }
sdrplay_api_ErrT sdrplay_api_LockDeviceApi() { if (result("lock") == sdrplay_api_Success) ++locked; return result("lock"); }
sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi() { ++unlocked; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT* devices, unsigned* count, unsigned) {
  *count = failAt == "none" ? 0 : 1;
  devices[0] = {}; devices[0].hwVer = failAt == "wrong-model" ? SDRPLAY_RSP1_ID : SDRPLAY_RSPduo_ID;
  std::strcpy(devices[0].SerNo, "mock-only");
  return result("enumerate");
}
sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT* device) {
  assert(device->tuner == sdrplay_api_Tuner_Both);
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
  return result(tuner == sdrplay_api_Tuner_A ? "gain-a" : "gain-b");
}
// No IQ buffers are used by this fixture; these satisfy the linked callback
// methods, and must never execute during an injected startup failure.
void IqData::lock() { assert(false); }
void IqData::unlock() { assert(false); }
void IqData::push_back(std::complex<double>) { assert(false); }

int main() {
  bool record = false;
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
  std::cout << "RSPduo mocked API: 15 failure stages, dual-tuner assignments, callback context and idempotent cleanup passed. No hardware opened.\n";
}
