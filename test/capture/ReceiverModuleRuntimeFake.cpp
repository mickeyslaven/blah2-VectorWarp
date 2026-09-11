#include <cstdio>
#include <cstdlib>
namespace {
void event(const char* step) {
  const char* path = std::getenv("BLAH2_RECEIVER_TEST_EVENTS");
  if (!path) return;
  FILE* file = std::fopen(path, "a");
  if (!file) std::abort();
  std::fprintf(file, "runtime:%s\n", step);
  std::fclose(file);
}
struct Lifetime {
  Lifetime() { event("loaded"); }
  ~Lifetime() { event("unloaded"); }
} lifetime;
}
extern "C" int receiver_fixture_runtime() { return 1; }
