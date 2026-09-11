#include "MchqReader.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>

int main() try {
  char temporary[]="/tmp/blah2-reader-test.XXXXXX";
  if (!mkdtemp(temporary)) throw std::runtime_error("Cannot create test directory");
  const auto path=std::filesystem::path(temporary)/"fixture.mchq";
  auto cleanup=[&] { std::filesystem::remove(path); std::filesystem::remove(temporary); };
  try {
    std::vector<uint8_t> wire;
    auto integer=[&](uint32_t value) { for (int n=3;n>=0;--n) wire.push_back(value>>(n*8)); };
    for (unsigned packet=0;packet<2;++packet) {
      for (auto v : {0x4d434851U,2U,32U,4U,0U,0U,0U,0U}) integer(v);
      for (unsigned ch=0;ch<2;++ch) for (float value : {527000000.0f,49.6f}) {
        const auto* p=reinterpret_cast<const uint8_t*>(&value); wire.insert(wire.end(),p,p+4);
      }
      for (unsigned ch=0;ch<2;++ch) for (unsigned sample=0;sample<32;++sample) {
        wire.push_back(96+sample+ch*3); wire.push_back(160-sample+ch*2);
      }
    }
    { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(wire.data()),wire.size()); }
    MchqReader reader(path,2,527000000); std::vector<std::deque<std::complex<double>>> values;
    for (unsigned frame=0;frame<3;++frame) {
      if (!reader.read(20,values)) throw std::runtime_error("Unexpected EOF");
      for (unsigned ch=0;ch<2;++ch) for (unsigned i=0;i<20;++i) {
        const double sample=(frame*20+i)%32;
        const std::complex<double> expected((sample-15.5)/127.5,(15.5-sample)/127.5);
        if (std::abs(values[ch][i]-expected)>1e-7) throw std::runtime_error("Sample alignment/value differs");
      }
    }
    if (reader.read(20,values) || reader.samples!=60 || reader.tailSamples!=4 || reader.packets!=2)
      throw std::runtime_error("EOF/tail counts differ");
    wire.pop_back();
    { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(wire.data()),wire.size()); }
    bool rejected=false;
    try { MchqReader truncated(path,2,527000000); truncated.read(64,values); }
    catch (const std::runtime_error&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Accepted truncated packet");
    cleanup(); std::cout << "Reader: sample values/alignment, packet crossing, EOF/tail, truncation passed\n";
  } catch (...) { cleanup(); throw; }
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
