// Owned subprocess fixture: the Python test kills this parent while its worker
// is stuck in initialization or frame execution, bypassing normal cleanup.
#include "process/ambiguity/GpuProcess.h"
#include <iostream>

int main(int argc, char** argv) try {
  if (argc != 2) return 2;
  blah2::GpuProcessOptions options;
  options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
  options.argument = argv[1];
  const blah2::GpuGeometry geometry{8, 4, 2, 1, 0};
  auto worker = blah2::createGpuProcess(geometry, "auto", options);
  std::vector<std::complex<float>> input(32, {1, 0}), output;
  worker->process(input, input, output);
  return 1; // The injected worker must not return.
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
