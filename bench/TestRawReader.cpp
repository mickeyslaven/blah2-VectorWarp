#include "BenchmarkReader.h"
#include <cstdio>
#include <iostream>
#include <unistd.h>

int main() try {
  char path[] = "/tmp/vectorwarp-raw-reader.XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("Cannot create fixture");
  close(fd);
  struct Cleanup { const char* p; ~Cleanup() { std::remove(p); } } cleanup{path};
  const std::vector<int> integers = {-32768,32767,0,-1, 1,-2,8192,-8192, 12,34,56,78};
  std::vector<unsigned char> bytes;
  for (int value : integers) { bytes.push_back(value & 255); bytes.push_back((value >> 8) & 255); }
  { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  BenchmarkReader reader(path,2,551000000,"rspduo-s16le");
  std::vector<std::deque<std::complex<double>>> output;
  if (!reader.read(2,output) || output.size()!=2 || output[0].size()!=2 ||
      output[0][0] != std::complex<double>(-32768,32767) ||
      output[1][0] != std::complex<double>(0,-1) ||
      output[0][1] != std::complex<double>(1,-2) ||
      output[1][1] != std::complex<double>(8192,-8192))
    throw std::runtime_error("Raw channel layout or signed normalization changed");
  if (reader.read(2,output) || reader.samples!=2 || reader.tailSamples!=1)
    throw std::runtime_error("Partial CPI counted as a complete CPI");
  bool rejected=false;
  try { BenchmarkReader invalid(path,3,551000000,"rspduo-s16le"); }
  catch (const std::invalid_argument&) { rejected=true; }
  if (!rejected) throw std::runtime_error("Accepted wrong raw channel count");
  { std::ofstream file(path,std::ios::binary|std::ios::app); file.put('x'); }
  rejected=false;
  try { BenchmarkReader invalid(path,2,551000000,"rspduo-s16le"); }
  catch (const std::runtime_error&) { rejected=true; }
  if (!rejected) throw std::runtime_error("Accepted truncated raw sample pair");
  std::cout << "Raw reader: signed values, channels, tail, and truncation passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
