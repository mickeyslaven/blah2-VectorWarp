#include "core/TrackGuidedShadow.private.h"
#include "core/TrackBeforeDetect.private.h"
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main()try{
 constexpr uint32_t rows=5,columns=17,guard=1,training=4;
 const std::vector<double> doppler{-4,-2,0,2,4};
 std::vector<double> old(rows*columns,1),now=old;
 const double alpha=track_before_detect::cfarAlpha(1e-4,8,2);
 old[1*columns+7]=12;now[2*columns+8]=12;
 auto r=track_guided_shadow::evaluate(now,old,rows,columns,0,doppler,8,0,7,-2,guard,training,alpha);
 require(r.valid&&r.aboveThreshold&&r.cut==12,"motion-aligned weak path was not recovered");
 r=track_guided_shadow::evaluate(now,old,rows,columns,0,doppler,8,0,8,0,guard,training,alpha);
 require(r.valid&&!r.aboveThreshold,"incompatible stationary path was incorrectly recovered");
 now[2*columns+8]=std::numeric_limits<double>::quiet_NaN();
 r=track_guided_shadow::evaluate(now,old,rows,columns,0,doppler,8,0,7,-2,guard,training,alpha);
 require(!r.valid,"non-finite power was admitted");
 now.assign(rows*columns,1);old.assign(rows*columns,1);old[1*columns]=12;now[2*columns+8]=12;
 r=track_guided_shadow::evaluate(now,old,rows,columns,0,doppler,8,0,0,-2,guard,training,alpha);
 require(!r.valid,"edge path with incomplete translated training was admitted");
 require(track_guided_shadow::nearestRow(doppler,-1)==1&&track_guided_shadow::nearestRow(doppler,1)==2,
  "nearest-row tie policy differs from GPU implementation");
 std::cout<<"TRACK_GUIDED_SHADOW_PASS aligned_recovered=1 incompatible_rejected=1 nonfinite_rejected=1 edge_rejected=1\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL track-guided-shadow: "<<e.what()<<'\n';return 1;}
