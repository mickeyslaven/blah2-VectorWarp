#pragma once
#include <cstdint>
#include <stdexcept>

namespace blah2::temporal_candidate {
// ABI16: bits26..29 encode 1 + (sourceColumn-keyColumn+1) +
// 3*(sourceRow-keyRow+1). Zero is reserved for the non-merged 1/2-CPI path.
// Association retains its finalized grid key; sparse complex evidence samples
// the original CFAR cell, including deterministic half-bin deduplication ties.
inline uint32_t sourceCell(uint32_t key,uint32_t flags,uint32_t rows,
    uint32_t columns,uint32_t maximumCpis){
 if(!rows||!columns||uint64_t(key)>=uint64_t(rows)*columns)
  throw std::invalid_argument("Resident temporal source grid invalid");
 const uint32_t code=(flags>>26)&15u;
 if(maximumCpis<=2){
  if(maximumCpis==2&&code)
   throw std::invalid_argument("Two-CPI candidate unexpectedly remapped its source cell");
  return key;
 }
 if((maximumCpis!=4&&maximumCpis!=8)||code<1||code>9)
  throw std::invalid_argument("Resident temporal source-cell offset missing or invalid");
 const int64_t row=int64_t(key/columns)+int((code-1)/3)-1,
  column=int64_t(key%columns)+int((code-1)%3)-1;
 if(row<0||column<0||row>=rows||column>=columns)
  throw std::invalid_argument("Resident temporal source-cell offset leaves grid");
 return uint32_t(row*columns+column);
}
}
