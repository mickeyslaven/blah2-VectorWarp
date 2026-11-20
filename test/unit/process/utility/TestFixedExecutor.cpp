#include "process/utility/FixedExecutor.h"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

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

TEST_CASE("borrowed executor work cancels and joins without growing its worker set")
{
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    CAPTURE(attempt);
    blah2::utility::FixedExecutor executor(3);
    std::atomic<unsigned> inFlight{0}, visits{0};
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::future<void> releaseTask;
    REQUIRE_THROWS_AS(executor.borrow(512, [&](std::size_t index) {
      ++inFlight; ++visits;
      if (!index) {
        while (inFlight.load() != executor.size()) std::this_thread::yield();
        // Both peers wait below, so only this worker can run the queued release
        // after its exception has reached the executor's cancellation handler.
        releaseTask = executor.submit([&] { release.set_value(); });
        --inFlight;
        throw std::runtime_error("injected tile");
      }
      gate.wait();
      --inFlight;
    }, [] {}), std::runtime_error);
    releaseTask.get();
    REQUIRE(inFlight.load() == 0);
    REQUIRE(visits.load() < 512);
    REQUIRE(visits.load() == executor.size());
    REQUIRE(executor.size() == std::size_t(3));
    std::atomic<unsigned> completed{0};
    REQUIRE_NOTHROW(executor.borrow(31, [&](std::size_t) { ++completed; }, [] {}));
    REQUIRE(completed.load() == 31);
    auto nested = executor.submit([&] { executor.borrow(1, [](std::size_t) {}, [] {}); });
    REQUIRE_THROWS_AS(nested.get(), std::logic_error);
  }
}

TEST_CASE("borrowed executor contention expires without enqueuing stale work")
{
  blah2::utility::FixedExecutor executor(2);
  std::promise<void> release; auto gate = release.get_future().share();
  std::atomic<unsigned> started{0}, borrowed{0};
  auto first = executor.submit([&] { ++started; gate.wait(); });
  auto second = executor.submit([&] { ++started; gate.wait(); });
  while (started.load() != 2) std::this_thread::yield();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
  bool expired = false;
  try {
    executor.borrow(8, [&](std::size_t) { ++borrowed; }, [&] {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("expired admission");
    });
  } catch (const std::runtime_error&) { expired = true; }
  release.set_value(); first.get(); second.get();
  REQUIRE(expired);
  REQUIRE(borrowed.load() == 0);
  REQUIRE_NOTHROW(executor.borrow(1, [&](std::size_t) { ++borrowed; }, [] {}));
  REQUIRE(borrowed.load() == 1);
}
