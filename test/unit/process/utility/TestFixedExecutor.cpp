#include "process/utility/FixedExecutor.h"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("fixed executor validates its bounded worker set")
{
  REQUIRE_THROWS_AS(blah2::utility::FixedExecutor(0), std::invalid_argument);
  blah2::utility::FixedExecutor executor(3);
  REQUIRE(executor.size() == 3);
  REQUIRE_NOTHROW(executor.parallel_for(0, [](std::size_t) {}));
  REQUIRE_THROWS_AS(executor.parallel_for(1, {}), std::invalid_argument);
}

TEST_CASE("fixed executor completes a failed batch and remains reusable")
{
  blah2::utility::FixedExecutor executor(3);
  std::vector<std::atomic<unsigned>> visits(17);
  for (auto& visit : visits) visit.store(0);
  REQUIRE_THROWS_AS(executor.parallel_for(visits.size(),
    [&](std::size_t index) {
      ++visits[index];
      if (index == 5) throw std::runtime_error("injected executor failure");
    }), std::runtime_error);
  for (const auto& visit : visits) REQUIRE(visit.load() == 1);

  std::atomic<unsigned> sum{0};
  REQUIRE_NOTHROW(executor.parallel_for(11,
    [&](std::size_t index) { sum += unsigned(index + 1); }));
  REQUIRE(sum.load() == 66);
  bool called = false;
  REQUIRE_NOTHROW(executor.parallel_for(1,
    [&](std::size_t index) { called = index == 0; }));
  REQUIRE(called);

  std::vector<std::string> indexed(8);
  executor.parallel_for(indexed.size(), [&](std::size_t index) {
    indexed[index] = std::to_string(index);
  });
  std::string ordered;
  for (const auto& value : indexed) ordered += value;
  REQUIRE(ordered == "01234567");

  // Regression: the final worker must finish notifying the stack-owned batch
  // before parallel_for returns and destroys it.
  std::atomic<unsigned> repeated{0};
  for (std::size_t batch = 0; batch < 2000; ++batch)
    executor.parallel_for(8,
      [&](std::size_t) { repeated.fetch_add(1, std::memory_order_relaxed); });
  REQUIRE(repeated.load() == 16000);
}
