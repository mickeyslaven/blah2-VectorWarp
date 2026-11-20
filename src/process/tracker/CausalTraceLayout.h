#pragma once
#include <cstdint>
// Optional diagnostic ABI, never checkpoint/inference state. Deterministic
// prefixes, not a claim of full-population coverage. At most 8,256 bytes/CPI.
namespace blah2::causal_trace {
constexpr uint32_t schema=1, prefix=32, headerWords=16, recordWords=32;
constexpr uint32_t words=headerWords+2*prefix*recordWords;
constexpr uint32_t bytes=words*4;
}
