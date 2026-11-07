#pragma once
#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/GpuWidebandChannelizer.h"
#include <functional>

namespace blah2::wideband_gpu
{
using Factory=std::function<std::unique_ptr<Backend>(const Config&,const std::string&)>;
std::unique_ptr<Backend> createProcess(const Config&,const std::string&,
  const GpuProcessOptions& = {});
int runWorker(const Factory&);
}
