// Link with RspDuo.cpp, Source.cpp and Recording.cpp; never link SDRplay's
// runtime library. These SDK functions are local failure-injection stubs.
#include "capture/rspduo/RspDuo.h"
#include "capture/PairedCpiQueue.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <thread>
#include <rapidjson/document.h>
extern std::atomic<bool> run_fg;
extern "C" const char* blah2_rspduo_startup_receipt_json_v1(const Source*) noexcept;

// Access only the synchronization points needed to force an overlapping pair.
// Redefining `private` also rewrites libstdc++ internals on GCC and is invalid.
struct RspDuoTestAccess {
  static IqData& reference(RspDuo& receiver) { return *receiver.outputBuffer1; }
  static bool pairClaimed(RspDuo& receiver) {
    std::lock_guard<std::mutex> lock(receiver.callbackMutex);
    return receiver.pendingCount == 0 && receiver.callbackSlotBusy[0];
  }
  static bool processing(RspDuo& receiver) {
    if (!receiver.callbackBProcessingMutex.try_lock()) return true;
    receiver.callbackBProcessingMutex.unlock();
    return false;
  }
};

namespace {
std::string failAt;
unsigned opened, closed, locked, unlocked, selected, released, initialized, uninitialized;
sdrplay_api_DevParamsT deviceParameters{};
sdrplay_api_RxChannelParamsT tunerA{}, tunerB{};
sdrplay_api_DeviceParamsT parameters{&deviceParameters, &tunerA, &tunerB};
bool nullParameters = false, removeOnInit = false, multipleDevices = false, stopAfterGain = false;
enum class CallbackScenario { None, Matched, BFirst, DuplicateA, LengthMismatch, EpochMismatch, Wraparound, Reset, ResetAfterPair, Removed, GapAfterPair, MalformedA, OversizedB, ConcurrentPairHandoff, ScaledCounterInvalidRaw, ScaledFsChangedA, ScaledFsChangedB };
CallbackScenario callbackScenario = CallbackScenario::None;
std::string selectedSerial;
std::string mockSerial = "mock-only";
void* callbackContext = nullptr;
rapidjson::Document receipt(const RspDuo& receiver) {
  rapidjson::Document parsed;
  parsed.Parse(receiver.startup_receipt_json().c_str());
  assert(!parsed.HasParseError() && parsed.IsObject());
  return parsed;
}
sdrplay_api_ErrT result(const char* stage) { return failAt == stage ? sdrplay_api_Fail : sdrplay_api_Success; }
void reset() {
  opened = closed = locked = unlocked = selected = released = initialized = uninitialized = 0;
  deviceParameters = {}; tunerA = {}; tunerB = {};
  parameters = {&deviceParameters, &tunerA, &tunerB};
  failAt.clear(); nullParameters = removeOnInit = multipleDevices = stopAfterGain = false;
  callbackScenario = CallbackScenario::None;
  selectedSerial.clear(); mockSerial = "mock-only"; callbackContext = nullptr;
}
}

sdrplay_api_ErrT sdrplay_api_Open() { if (result("open") == sdrplay_api_Success) ++opened; return result("open"); }
sdrplay_api_ErrT sdrplay_api_Close() { ++closed; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_ApiVersion(float* version) {
  *version = failAt == "version-mismatch" ? 1.0f :
    failAt == "version-unusual" ? 101.0f : SDRPLAY_API_VERSION;
  return result("version");
}
sdrplay_api_ErrT sdrplay_api_LockDeviceApi() { if (result("lock") == sdrplay_api_Success) ++locked; return result("lock"); }
sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi() { ++unlocked; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT* devices, unsigned* count, unsigned) {
  *count = failAt == "none" ? 0 : (multipleDevices ? 2 : 1);
  devices[0] = {}; devices[0].hwVer = failAt == "wrong-model" ? SDRPLAY_RSP1_ID : SDRPLAY_RSPduo_ID;
  std::strcpy(devices[0].SerNo, mockSerial.c_str());
  if (failAt == "bad-serial") std::strcpy(devices[0].SerNo, "bad\nserial");
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
  short ax[] = {1, 3, 5}, aq[] = {2, 4, 6}, bx[] = {7, 9, 11}, bq[] = {8, 10, 12};
  short ax2[] = {21, 23, 25}, aq2[] = {22, 24, 26}, bx2[] = {27, 29, 31}, bq2[] = {28, 30, 32};
  auto callA = [&](uint32_t first, unsigned count = 2, unsigned reset = 0, bool fsChanged = false) {
    sdrplay_api_StreamCbParamsT params{}; params.firstSampleNum = first; params.fsChanged = fsChanged;
    callbacks->StreamACbFn(ax, aq, &params, count, reset, context);
  };
  auto callB = [&](uint32_t first, unsigned count = 2, unsigned reset = 0, bool fsChanged = false) {
    sdrplay_api_StreamCbParamsT params{}; params.firstSampleNum = first; params.fsChanged = fsChanged;
    callbacks->StreamBCbFn(bx, bq, &params, count, reset, context);
  };
  auto callA2 = [&](uint32_t first) {
    sdrplay_api_StreamCbParamsT params{}; params.firstSampleNum = first;
    callbacks->StreamACbFn(ax2, aq2, &params, 2, 0, context);
  };
  auto callB2 = [&](uint32_t first) {
    sdrplay_api_StreamCbParamsT params{}; params.firstSampleNum = first;
    callbacks->StreamBCbFn(bx2, bq2, &params, 2, 0, context);
  };
  switch (callbackScenario) {
    case CallbackScenario::Matched: callA(100); callB(100); run_fg = false; break;
    case CallbackScenario::BFirst: callB(100); callA(100); callB(100); run_fg = false; break;
    case CallbackScenario::DuplicateA: callA(100); callB(100); callA(102); callA(102); callB(102); break;
    case CallbackScenario::LengthMismatch: callA(100); callB(100); callA(102); callB(102, 3); callA(105); callB(105); break;
    case CallbackScenario::EpochMismatch: callA(100); callB(100); callA(102); callB(103); callA(104); callB(104); break;
    case CallbackScenario::Wraparound: callA(UINT32_MAX - 1); callB(UINT32_MAX - 1); callA(0); callB(0); run_fg = false; break;
    case CallbackScenario::Reset: callbacks->StreamACbFn(nullptr, nullptr, nullptr, 0, 1, context); callA(100); callB(100); run_fg = false; break;
    case CallbackScenario::MalformedA: callbacks->StreamACbFn(nullptr, aq, nullptr, 0, 0, context); break;
    case CallbackScenario::OversizedB: callB(100, 262145); break;
    case CallbackScenario::ScaledCounterInvalidRaw: callA(UINT32_MAX); break;
    case CallbackScenario::ScaledFsChangedA: callA(100, 2, 0, true); break;
    case CallbackScenario::ScaledFsChangedB: callA(100); callB(100, 2, 0, true); break;
    case CallbackScenario::ResetAfterPair: callA(100); callB(100); callA(102, 2, 1); callA(102); callB(102); break;
    case CallbackScenario::GapAfterPair: callA(100); callB(100); callA(102); callA(104); callB(104); break;
    case CallbackScenario::Removed: { sdrplay_api_EventParamsT event{}; callbacks->EventCbFn(sdrplay_api_DeviceRemoved, sdrplay_api_Tuner_Both, &event, context); break; }
    case CallbackScenario::ConcurrentPairHandoff: {
      auto* receiver = static_cast<RspDuo*>(context);
      callA(100);
      RspDuoTestAccess::reference(*receiver).lock();
      std::thread first([&] { callB(100); });
      auto captured = [&] {
        return RspDuoTestAccess::pairClaimed(*receiver);
      };
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!captured() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      assert(captured());
      // B1 has claimed its pair slot and is blocked on outputBuffer1.  This
      // directly proves that it owns the B processing lock before B2 starts;
      // output order below then cannot depend on mutex waiter fairness.
      assert(RspDuoTestAccess::processing(*receiver));
      callA2(102);
      // A may deliver another contiguous block while B1 is still publishing.
      // Both pending A blocks must remain paired with their matching B block.
      callA2(104);
      std::thread second([&] { callB2(102); });
      RspDuoTestAccess::reference(*receiver).unlock();
      first.join(); second.join(); callB2(104); run_fg = false;
      break;
    }
    case CallbackScenario::None: break;
  }
  return result("init");
}
sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE) { ++uninitialized; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_Update(HANDLE, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T) {
  if (stopAfterGain && tuner == sdrplay_api_Tuner_B) run_fg = false;
  return result(tuner == sdrplay_api_Tuner_A ? "gain-a" : "gain-b");
}
int main() {
  bool record = false;
  for (uint32_t rate : {2000000u, 1000000u, 500000u, 250000u, 125000u, 62500u}) {
    for (int agc : {0, 5, 50, 100}) {
      reset(); multipleDevices = true; stopAfterGain = true;
      RspDuo receiver("RspDuo", 527000000, rate, "/unused", &record,
        -37, agc, 25, 47, 5, agc != 0, agc == 0, "mock-second");
      assert(receiver.startup_receipt_json().empty());
      receiver.start();
      const auto started = receipt(receiver);
      assert(std::strcmp(started["status"].GetString(), "pending") == 0);
      assert(started["sdk"]["stages"]["getDeviceParams"].GetBool());
      assert(!started["sdk"]["stages"]["init"].GetBool());
      assert(std::strcmp(started["selected"]["serial"].GetString(), "mock-second") == 0);
      assert(started["requested"]["sampleRate"].GetUint() == rate);
      assert(started["requested"]["gainReduction"][0].GetInt() == 25);
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
      assert(deviceParameters.mode == sdrplay_api_ISOCH);
      receiver.process(nullptr, nullptr); // Fake SDK stops the test loop after both accepted Updates.
      const auto accepted = receipt(receiver);
      rapidjson::Document exported;
      exported.Parse(blah2_rspduo_startup_receipt_json_v1(&receiver));
      assert(!exported.HasParseError() && exported["status"] == "accepted");
      assert(std::strcmp(accepted["status"].GetString(), "accepted") == 0);
      assert(accepted["hardwareVerified"].GetBool() == false);
      assert(accepted["readbackAvailable"].GetBool() == false);
      assert(accepted["sdk"]["stages"]["gainUpdateA"].GetBool());
      assert(accepted["sdk"]["stages"]["gainUpdateB"].GetBool());
      assert(initialized == 1 && callbackContext == &receiver);
      receiver.stop(); receiver.stop();
      assert(opened == closed && locked == unlocked && selected == released && initialized == uninitialized);
    }
  }
  struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  reset(); mockSerial = "mock-\"\\serial"; stopAfterGain = true;
  RspDuo escaped("RspDuo", 204640000, 2000000, "/unused", &record,
    -30, 50, 30, 31, 3, true, false, mockSerial);
  escaped.start(); escaped.process(nullptr, nullptr);
  const auto previousLocale = std::locale::global(
    std::locale(std::locale::classic(), new CommaDecimal));
  const auto escapedReceipt = receipt(escaped);
  std::locale::global(previousLocale);
  assert(std::strcmp(escapedReceipt["status"].GetString(), "accepted") == 0);
  assert(std::string(escapedReceipt["requested"]["serial"].GetString()) == mockSerial);
  assert(std::string(escapedReceipt["selected"]["serial"].GetString()) == mockSerial);
  assert(escapedReceipt["sdk"]["version"].IsNumber());
  escaped.stop();
  reset(); stopAfterGain = true;
  setenv("VECTORWARP_RSPDUO_USB_MODE", "bulk", 1);
  RspDuo bulk("RspDuo", 204640000, 2000000, "/unused", &record,
    -30, 50, 30, 31, 3, true, false);
  bulk.start();
  assert(deviceParameters.mode == sdrplay_api_BULK);
  bulk.process(nullptr, nullptr); bulk.stop();
  unsetenv("VECTORWARP_RSPDUO_USB_MODE");
  setenv("VECTORWARP_RSPDUO_COUNTER_SCALE", "2", 1);
  try {
    RspDuo invalidScale("RspDuo", 204640000, 2000000, "/unused", &record,
      -30, 50, 30, 31, 3, true, false);
    assert(false);
  } catch (const std::invalid_argument&) {}
  unsetenv("VECTORWARP_RSPDUO_COUNTER_SCALE");
  reset(); stopAfterGain = true; deviceParameters.fsFreq.fsHz = 6000000;
  setenv("VECTORWARP_RSPDUO_COUNTER_SCALE", "3", 1);
  RspDuo scaled("RspDuo", 204640000, 2000000, "/unused", &record,
    -30, 50, 30, 31, 3, true, false);
  scaled.start(); scaled.process(nullptr, nullptr); scaled.stop();
  reset(); deviceParameters.fsFreq.fsHz = 6000000;
  RspDuo invalidScaledRate("RspDuo", 204640000, 1000000, "/unused", &record,
    -30, 50, 30, 31, 3, true, false);
  try { invalidScaledRate.start(); assert(false); } catch (const std::runtime_error&) {}
  invalidScaledRate.stop();
  unsetenv("VECTORWARP_RSPDUO_COUNTER_SCALE");
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
  assert(tooFast.startup_receipt_json().empty());
  assert(opened == 0); // Six-MS/s output is not offered by this dual-tuner adapter.
  for (const auto& stage : {"open", "version", "version-mismatch", "version-unusual", "lock", "enumerate", "none", "wrong-model", "bad-serial", "select", "debug", "parameters"}) {
    reset(); failAt = stage;
    RspDuo receiver("RspDuo", 204640000, 2000000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
    bool failed = false;
    try { receiver.start(); } catch (const std::exception& error) { failed = true; assert(std::string(error.what()).find("[RspDuo]") != std::string::npos); }
    assert(failed); receiver.stop(); receiver.stop();
    const auto incomplete = receipt(receiver);
    assert(std::strcmp(incomplete["status"].GetString(), "pending") == 0);
    if (stage == std::string("version-mismatch")) {
      assert(!incomplete["sdk"]["stages"]["apiVersion"].GetBool());
      assert(incomplete["sdk"]["version"].GetFloat() == 1.0f);
    }
    if (stage == std::string("version-unusual"))
      assert(incomplete["sdk"]["version"].IsNull());
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
    const auto incomplete = receipt(receiver);
    assert(std::strcmp(incomplete["status"].GetString(),
      stage == std::string("removed") ? "accepted" : "pending") == 0);
    if (stage == std::string("gain-b")) {
      assert(incomplete["sdk"]["stages"]["init"].GetBool());
      assert(incomplete["sdk"]["stages"]["gainUpdateA"].GetBool());
      assert(!incomplete["sdk"]["stages"]["gainUpdateB"].GetBool());
    }
    receiver.stop(); receiver.stop();
    assert(opened == closed && locked == unlocked && selected == released && initialized == uninitialized);
  }
  // Published SDRplay API 3.09 section 5, 50-ohm RSPduo LNA boundaries.
  for (const auto& candidate : {
      std::pair<uint32_t, int>{59999999, 6}, {60000000, 9},
      {999999999, 9}, {1000000000, 8}, {2000000000, 0}}) {
    reset();
    RspDuo receiver("RspDuo", candidate.first, 2000000, "/unused", &record,
      -30, 50, 30, 31, candidate.second, false, false);
    receiver.start(); receiver.stop();
    assert(opened == closed);
  }
  for (const auto& candidate : {
      std::pair<uint32_t, int>{59999999, 7}, {1000000000, 9}}) {
    reset();
    RspDuo receiver("RspDuo", candidate.first, 2000000, "/unused", &record,
      -30, 50, 30, 31, candidate.second, false, false);
    try { receiver.start(); assert(false); } catch (const std::invalid_argument&) {}
    assert(receiver.startup_receipt_json().empty());
    assert(opened == 0);
  }
  for (const auto& serial : {std::string("bad\nserial"), std::string(100, 'x')}) {
    reset();
    RspDuo receiver("RspDuo", 204640000, 2000000, "/unused", &record,
      -30, 50, 30, 31, 3, false, false, serial);
    try { receiver.start(); assert(false); } catch (const std::invalid_argument&) {}
    assert(receiver.startup_receipt_json().empty() && opened == 0);
  }
  for (const auto scenario : {CallbackScenario::Matched, CallbackScenario::Wraparound,
    CallbackScenario::BFirst, CallbackScenario::Reset, CallbackScenario::DuplicateA,
    CallbackScenario::LengthMismatch, CallbackScenario::EpochMismatch, CallbackScenario::ResetAfterPair,
    CallbackScenario::Removed, CallbackScenario::GapAfterPair, CallbackScenario::MalformedA, CallbackScenario::OversizedB,
    CallbackScenario::ConcurrentPairHandoff, CallbackScenario::ScaledCounterInvalidRaw,
    CallbackScenario::ScaledFsChangedA, CallbackScenario::ScaledFsChangedB}) {
    reset(); callbackScenario = scenario;
    if (scenario == CallbackScenario::ScaledCounterInvalidRaw ||
        scenario == CallbackScenario::ScaledFsChangedA || scenario == CallbackScenario::ScaledFsChangedB) {
      deviceParameters.fsFreq.fsHz = 6000000;
      setenv("VECTORWARP_RSPDUO_COUNTER_SCALE", "3", 1);
    }
    RspDuo receiver("RspDuo", 204640000, 2000000, "/unused", &record, -30, 50, 30, 31, 3, true, true);
    receiver.start(); IqData reference(16), surveillance(16);
    bool failed = false; std::string message;
    try { receiver.process(&reference, &surveillance); }
    catch (const std::exception& error) { failed = true; message = error.what(); }
    const bool matched = scenario == CallbackScenario::Matched || scenario == CallbackScenario::Wraparound ||
      scenario == CallbackScenario::BFirst || scenario == CallbackScenario::Reset ||
      scenario == CallbackScenario::ConcurrentPairHandoff;
    assert(failed != matched);
    if (matched) {
      const auto a = reference.get_data(), b = surveillance.get_data();
      const unsigned expected = scenario == CallbackScenario::ConcurrentPairHandoff ? 6 :
        scenario == CallbackScenario::Wraparound ? 4 : 2;
      assert(a.size() == expected && b.size() == expected);
      assert(a.front() == std::complex<double>(1, 2));
      assert(b.front() == std::complex<double>(7, 8));
      if (scenario == CallbackScenario::ConcurrentPairHandoff) {
        assert(a[2] == std::complex<double>(21, 22));
        assert(b[2] == std::complex<double>(27, 28));
        assert(a[4] == std::complex<double>(21, 22));
        assert(b[4] == std::complex<double>(27, 28));
      }
    } else if (scenario == CallbackScenario::Removed) {
      assert(message.find("Receiver disconnected") != std::string::npos);
      assert(reference.get_length() == 0 && surveillance.get_length() == 0);
    } else {
      assert(message.find("callback pairing fault") != std::string::npos);
      const unsigned expected = (scenario == CallbackScenario::GapAfterPair ||
        scenario == CallbackScenario::DuplicateA || scenario == CallbackScenario::LengthMismatch ||
        scenario == CallbackScenario::EpochMismatch || scenario == CallbackScenario::ResetAfterPair) ? 2 : 0;
      assert(reference.get_length() == expected && surveillance.get_length() == expected);
    }
    receiver.stop();
    if (scenario == CallbackScenario::ScaledCounterInvalidRaw ||
        scenario == CallbackScenario::ScaledFsChangedA || scenario == CallbackScenario::ScaledFsChangedB)
      unsetenv("VECTORWARP_RSPDUO_COUNTER_SCALE");
  }
  // The opt-in queue takes complete paired IIQQ blocks before any FIFO
  // conversion.  The mock SDK proves callback pairing and direct handoff
  // without opening an SDRplay runtime.
  reset(); callbackScenario = CallbackScenario::Matched;
  RspDuo queued("RspDuo", 204640000, 2000000, "/unused", &record,
    -30, 50, 30, 31, 3, true, true);
  PairedCpiQueue queue(2, 1);
  queued.set_paired_cpi_queue(&queue);
  queued.start(); queued.process(nullptr, nullptr); queued.stop(); queue.close();
  size_t queueSlot = 0; assert(queue.acquire(queueSlot));
  const auto& queuedBlock = queue.block(queueSlot);
  assert(queuedBlock.first == 0 && queuedBlock.iq[0] == 1 &&
    queuedBlock.iq[1] == 2 && queuedBlock.iq[2] == 7 && queuedBlock.iq[3] == 8);
  queue.release(queueSlot);
  std::cout << "RSPduo mocked API: dual-tuner settings, startup failures, and real-IQ callback pairing/reset/removal faults passed. No hardware opened.\n";
}
