#include "process/mixed/MixedAutoPolicy.h"
#include "process/mixed/MixedEligibility.h"
#include <cassert>
using blah2::mixed::AutoPolicy;
using blah2::mixed::Choice;
int main(){
  blah2::mixed::Shape shape{2000000,1000000,1,301,411,3322,4096,
    410,-10,-10,400,-300,300,0,false,true};
  assert(blah2::mixed::qualified(shape));
  assert(!blah2::mixed::autoCandidate("cpu",shape,true));
  assert(!blah2::mixed::autoCandidate("gpu",shape,true));
  assert(blah2::mixed::autoCandidate("auto",shape,true));
  assert(!blah2::mixed::autoCandidate("auto",shape,false));
  assert(!blah2::mixed::autoCandidate("auto",shape,true,"vulkan:1"));
  shape.ambiguityMin=-11;
  assert(!blah2::mixed::autoCandidate("auto",shape,true));
  AutoPolicy winner;
  assert(winner.choose(0,2000000,0)==Choice::shadow);
  winner.shadow(1e-7,2e-7);
  assert(winner.choose(0,2000000,0)==Choice::shadow);
  winner.shadow(1e-7,2e-7);
  for(int i=0;i<6;++i){
    const auto choice=winner.choose(0,2000000,0);
    assert(choice==(i%2?Choice::mixed:Choice::cpu));
    winner.complete(choice,i%2?340:380);
  }
  assert(winner.selected()&&winner.choose(0,2000000,0)==Choice::mixed);
  // One transient slow group does not discard a valid candidate. Two
  // successive groups losing the measured speed margin return to CPU.
  for(int i=0;i<5;++i)winner.complete(Choice::mixed,400);
  assert(winner.selected());
  for(int i=0;i<5;++i)winner.complete(Choice::mixed,340);
  assert(winner.selected());
  for(int i=0;i<10;++i)winner.complete(Choice::mixed,400);
  assert(winner.disabled()&&winner.choose(0,2000000,0)==Choice::cpu);
  AutoPolicy slower;
  slower.shadow(0,0);slower.shadow(0,0);
  for(int i=0;i<6;++i){
    const auto choice=slower.choose(0,2000000,0);
    slower.complete(choice,i%2?410:380);
  }
  assert(!slower.selected()&&slower.choose(0,2000000,0)==Choice::cpu);
  AutoPolicy invalid;
  invalid.shadow(0,1.1e-4);
  assert(invalid.disabled()&&invalid.choose(0,2000000,0)==Choice::cpu);
  AutoPolicy busy;
  for(int i=0;i<16;++i)assert(busy.choose(1500000,2000000,0)==Choice::cpu);
  assert(busy.disabled());
  AutoPolicy drops;
  assert(drops.choose(0,2000000,1)==Choice::cpu&&drops.disabled());
  AutoPolicy growth;
  assert(growth.choose(0,2000000,0)==Choice::shadow);
  assert(growth.choose(600000,2000000,0)==Choice::cpu);
  assert(!growth.disabled());
  // Recorded live regression: the first oracle left 935044 queued samples;
  // after one CPU CPI it still had 741444. A second oracle there lost 1M.
  AutoPolicy recovering;
  assert(recovering.choose(172304,2000000,0)==Choice::cpu);
  assert(recovering.choose(30000,2000000,0)==Choice::shadow);
  recovering.shadow(1e-6,2e-5);
  for(auto backlog:{935044u,741444u,500000u,260000u,125000u})
    assert(recovering.choose(backlog,2000000,0)==Choice::cpu);
  assert(!recovering.disabled());
  assert(recovering.choose(100000,2000000,0)==Choice::shadow);
}
