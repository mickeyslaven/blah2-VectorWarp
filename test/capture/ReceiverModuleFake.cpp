#include "capture/ReceiverModule.h"
#include "capture/Source.h"
#include "ReceiverCohort.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef FIXTURE_RECEIVER
#define FIXTURE_RECEIVER "Usrp"
#endif
namespace {
void event(const char* name) {
  const auto path = std::getenv("BLAH2_RECEIVER_TEST_EVENTS");
  if (!path) return;
  FILE* file = std::fopen(path, "a");
  if (!file) std::abort();
  std::fprintf(file, "%s:%s\n", FIXTURE_RECEIVER, name);
  std::fclose(file);
}
struct Lifetime {
  Lifetime() { event("loaded"); }
  ~Lifetime() { event("unloaded"); }
} lifetime;

class FakeSource : public Source {
public:
  explicit FakeSource(const Blah2ReceiverConfig& c)
    : Source("Usrp", c.frequency, c.sampleRate, c.recordingPath, c.saveIq) {
    if (c.frequency != 527000000 || c.sampleRate != 6000000 || c.channels != 2)
      std::abort();
    event("created");
  }
  ~FakeSource() override { event("destroyed"); }
  void start() override { event("started"); }
  void stop() override { event("stopped"); }
  void process(IqData* a, IqData* b) override {
    a->push_back({6, 1}); b->push_back({6, -1}); event("processed");
  }
};
Source* create(const Blah2ReceiverConfig* config, char* error, std::size_t capacity) noexcept {
#ifdef FIXTURE_CREATE_ERROR
  (void)config;
  if (error && capacity) std::snprintf(error, capacity, "%s", "fixture creation refused");
  return nullptr;
#else
  (void)error; (void)capacity;
  return new FakeSource(*config);
#endif
}
void destroy(Source* source) noexcept { source->stop(); delete source; }

#ifdef FIXTURE_BAD_ABI
constexpr unsigned abi = 999;
#else
constexpr unsigned abi = BLAH2_RECEIVER_ABI;
#endif
#ifdef FIXTURE_BAD_COHORT
const char* cohort = "different-source-layout";
#else
const char* cohort = BLAH2_RECEIVER_COHORT;
#endif
const Blah2ReceiverApi api{abi, sizeof(Blah2ReceiverApi), cohort, FIXTURE_RECEIVER, create, destroy};
}

#ifdef FIXTURE_MISSING_RUNTIME
extern "C" int receiver_fixture_runtime();
#endif
extern "C" const Blah2ReceiverApi* blah2_receiver_api_v1() noexcept {
#ifdef FIXTURE_MISSING_RUNTIME
  (void)receiver_fixture_runtime();
#endif
  return &api;
}
