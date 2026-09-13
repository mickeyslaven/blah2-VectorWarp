// Failure-only SDK substitute for local-adapter installation tests. No vendor
// runtime, receiver access, callback stream or radio operation is implemented.
#include <sdrplay_api.h>
#ifndef FIXTURE_MARKER
#define FIXTURE_MARKER 7
#endif
extern "C" int vectorwarp_runtime_marker() { return FIXTURE_MARKER; }
sdrplay_api_ErrT sdrplay_api_Open() { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_Close() { return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_ApiVersion(float* value) { *value = SDRPLAY_API_VERSION; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_LockDeviceApi() { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi() { return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT*, unsigned* count, unsigned) { *count=0; return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT*) { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT*) { return sdrplay_api_Success; }
const char* sdrplay_api_GetErrorString(sdrplay_api_ErrT) { return "offline installation fixture"; }
sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE, sdrplay_api_DbgLvl_t) { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE, sdrplay_api_DeviceParamsT**) { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_Init(HANDLE, sdrplay_api_CallbackFnsT*, void*) { return sdrplay_api_Fail; }
sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE) { return sdrplay_api_Success; }
sdrplay_api_ErrT sdrplay_api_Update(HANDLE, sdrplay_api_TunerSelectT,
    sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T) { return sdrplay_api_Fail; }
