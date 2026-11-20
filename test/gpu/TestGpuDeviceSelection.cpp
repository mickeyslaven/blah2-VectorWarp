#include "process/ambiguity/GpuDeviceSelection.h"
#include "process/ambiguity/GpuProcess.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template <typename Function>
void rejects(Function function, const std::string& expected) {
  try { function(); }
  catch (const std::exception& error) {
    require(std::string(error.what()).find(expected) != std::string::npos,
      "Unexpected device-selection refusal");
    return;
  }
  throw std::runtime_error("Invalid resident device selection was accepted");
}
const std::string mixed = R"({"version":1,"available":true,"qualification":"not-run","devices":[{"id":"9:2:0","name":"second FP64","shaderFloat64":true},{"id":"1:2:1","name":"first FP64","shaderFloat64":true},{"id":"0:2:2","name":"FP32","shaderFloat64":false}]})";
}

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::string mode = argv[1];
    if (mode == "--driver-status") { std::cout << mixed; return 0; }
    if (mode == "--oversize") { std::cout << std::string(32769, 'x'); return 0; }
    if (mode == "--hang") { std::this_thread::sleep_for(std::chrono::seconds(2)); return 0; }
    if (mode == "--fail") { std::cout << mixed; return 1; }
    return 2;
  }
  try {
    using blah2::selectResidentGpuDevice;
    require(selectResidentGpuDevice(mixed) == "1:2:1", "Automatic selection must sort exact FP64 IDs");
    require(selectResidentGpuDevice(mixed, "") == "1:2:1", "Unset selector differs from automatic");
    require(selectResidentGpuDevice(mixed, "9:2:0") == "9:2:0", "Explicit eligible selector was replaced");
    rejects([&] { selectResidentGpuDevice(mixed, "missing"); }, "unavailable");
    rejects([&] { selectResidentGpuDevice(mixed, "0:2:2"); }, "shaderFloat64=0");
    const std::string empty = R"({"version":1,"available":true,"qualification":"not-run","devices":[]})";
    rejects([&] { selectResidentGpuDevice(empty); }, "No GPU devices");
    const std::string fp32 = R"({"version":1,"available":true,"qualification":"not-run","devices":[{"id":"0:2:2","name":"FP32","shaderFloat64":false}]})";
    rejects([&] { selectResidentGpuDevice(fp32); }, "shaderFloat64=0");
    rejects([&] { selectResidentGpuDevice("{}"); }, "Invalid GPU driver status");
    rejects([&] { selectResidentGpuDevice(R"({"version":1,"available":false,"qualification":"not-run","devices":[]})"); }, "driver unavailable");
    auto duplicate = mixed;
    duplicate.replace(duplicate.find("1:2:1"), 5, "9:2:0");
    rejects([&] { selectResidentGpuDevice(duplicate); }, "Invalid GPU driver status");
    blah2::GpuProcessOptions options;
    options.executable = argv[0]; options.startupMs = 1000;
    require(selectResidentGpuDevice(blah2::queryGpuDriverStatus(options)) == "1:2:1",
      "Isolated query did not return the eligible identity");
    options.argument = "--oversize";
    rejects([&] { blah2::queryGpuDriverStatus(options); }, "output limit");
    options.argument = "--fail";
    rejects([&] { blah2::queryGpuDriverStatus(options); }, "query failed");
    options.argument = "--hang"; options.startupMs = 40;
    const auto start = std::chrono::steady_clock::now();
    rejects([&] { blah2::queryGpuDriverStatus(options); }, "timed out");
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
      "Failed query was not bounded and reaped");
    std::cout << "Resident device selection and bounded isolated enumeration pass.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
