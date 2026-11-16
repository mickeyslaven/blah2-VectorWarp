#!/usr/bin/env python3
"""Compile the production shader's pure lineage functions as bounded C++.

This verifies the actual integer contract, not Vulkan execution or RF PFA.
The separate --lineage-only GPU fixture covers production stage wiring.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
using uint=unsigned;
// PRODUCTION_FUNCTIONS
void check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
struct Ring {
 std::vector<uint> words;uint head=0,length=0;
 explicit Ring(uint capacity):words(capacity){}
 uint recent()const{uint result=0;
  for(uint age=1;age<=length&&age<=7;++age)
   if(lineageVote(words[(head+length-age)%words.size()],true))result|=1u<<(age-1);
  return result;}
 void append(uint word){uint slot=(head+length)%words.size();
  if(length<words.size())++length;else head=(head+1)%words.size();words[slot]=word;}
 uint votes(uint n,bool experimental)const{uint result=0;
  for(uint j=length>n?length-n:0;j<length;++j)
   result+=lineageVote(words[(head+j)%words.size()],experimental);
  return result;}
};
int main()try{
 unsigned cases=0;
 check(lineageCounterNext(3,0,4,0)&&lineageCounterNext(UINT32_MAX,0,0,1),"adjacent identity rejected");
 check(!lineageCounterNext(3,0,3,0)&&!lineageCounterNext(3,0,5,0)&&
  !lineageCounterNext(UINT32_MAX,UINT32_MAX,0,0),"duplicate/gap/overflow identity accepted");
 check(lineageTemporalNext(8,9)&&!lineageTemporalNext(8,8)&&!lineageTemporalNext(8,10)&&
  !lineageTemporalNext(8,0)&&!lineageTemporalNext(0xfffffffeu,0xfffffffeu),"ring identity gap/reset accepted");
 for(bool experimental:{false,true})for(uint state=0;state<4;++state)
  for(uint source:{0u,4u,8u})for(uint depth:{0u,1u,2u,4u,8u})for(bool credit:{false,true}){
   const auto word=lineageWord(state,source,depth,credit,experimental);
   const bool valid=(source==0?depth==0:source==8?depth==2:depth!=0)&&
    !(state==0&&source==8)&&!(state==3&&source!=0)&&!(experimental&&credit&&source==0)&&
    (experimental||source!=8);
   check(lineageWordValid(word,experimental)==valid,"history validation differs");
   check(lineageDepth(word,experimental)==depth,"support depth roundtrip differs");
   if(!experimental)check(word==(state|source|(depth<<4)),"ordinary schema3 bytes changed");
   ++cases;
  }
 for(uint depth:{1u,2u,4u,8u})for(uint mask=0;mask<128;++mask){bool expected=true;
  for(uint age=1;age<depth;++age)if(mask&(1u<<(age-1)))expected=false;
  check(lineageDisjoint(depth,mask)==expected,"window boundary differs");++cases;}
 for(uint invalid:{0u,3u,5u,9u,32u,UINT32_MAX})
  check(!lineageDisjoint(invalid,0),"invalid support accepted");
 for(uint capacity:{100u,255u}){
  Ring ring(capacity);std::set<int> support;
  for(int frame=0;frame<900;++frame){uint depth=std::array<uint,3>{2,4,8}[frame%3];
   bool expected=true;for(int i=frame-int(depth)+1;i<=frame;++i)if(support.count(i))expected=false;
   const bool credit=lineageDisjoint(depth,ring.recent());
   check(credit==expected,"wrapped ring reused an actual CPI");
   if(credit)for(int i=frame-int(depth)+1;i<=frame;++i)support.insert(i);
   while(!support.empty()&&*support.begin()<frame-8)support.erase(support.begin());
   ring.append(lineageWord(1,4,depth,credit,true));++cases;
  }
 }
 Ring mixed(100);for(uint frame=0;frame<7;++frame){const uint source=frame%2==0&&frame?8:4;
  bool credit=lineageDisjoint(2,mixed.recent());mixed.append(lineageWord(1,source,2,credit,true));
  check(mixed.votes(7,true)==frame/2+1,"mixed detector/guided windows double credited");
  if(frame==5)check(mixed.votes(6,true)<4,"disjoint 4-of-6 falsely became feasible");}
 check(mixed.votes(7,true)==4,"feasible 4-of-7 boundary failed");
 Ring impulse(100);impulse.append(lineageWord(0,4,2,true,true));
 for(unsigned frame=1;frame<8;++frame){const bool credit=lineageDisjoint(8,impulse.recent());
  impulse.append(lineageWord(1,4,8,credit,true));check(impulse.votes(8,true)==1,"one impulse manufactured persistence");}
 for(uint bits=0;bits<64;++bits){Ring ordinary(100);uint expected=0;
  for(uint i=0;i<6;++i){bool hit=(bits&(1u<<i))!=0;expected+=hit;
   ordinary.append(lineageWord(hit?1:0,hit?4:0,hit?8:0,hit,false));}
  check(ordinary.votes(6,false)==expected,"ordinary 4-of-6 semantics changed");++cases;}
 std::cout<<"PASS production lineage integer contract cases="<<cases
  <<" mixed_windows=1 impulse=1 boundaries=1 gap_reset_identity=1 wrap=1 ordinary_parity=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
'''


class TrackerLineageContract(unittest.TestCase):
    def test_production_integer_contract(self):
        source = (ROOT / "core/GpuTracker.private.h").read_text()
        match = re.search(r"// BEGIN PURE LINEAGE CONTRACT[^\n]*\n(.*?)"
                          r"// END PURE LINEAGE CONTRACT", source, re.S)
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory(prefix="tracker-lineage-") as temporary:
            directory = Path(temporary)
            source_file = directory / "contract.cpp"
            source_file.write_text(DRIVER.replace("// PRODUCTION_FUNCTIONS", match[1]))
            binary = directory / "contract"
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                       "-Werror", "-O1", str(source_file), "-o", str(binary)]
            if os.environ.get("LINEAGE_SANITIZE") == "1":
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=30)
            completed = subprocess.run([str(binary)], check=True, text=True,
                                       capture_output=True, timeout=10)
            self.assertIn("ordinary_parity=1", completed.stdout)
            print(completed.stdout, end="")


if __name__ == "__main__":
    unittest.main()
