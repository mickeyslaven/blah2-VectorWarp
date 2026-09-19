#include "data/IqData.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
template<class F> void rejects(F action) {
  try { action(); } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("Out-of-range FIFO operation accepted");
}
}
int main() {
  try {
    // Every split, including the ownership-transfer path with a short tail.
    for (unsigned length = 0; length <= 80; ++length) {
      for (unsigned count = 0; count <= length; ++count) {
        IqData split(80);
        for (unsigned i = 0; i < length; ++i) split.push_back({double(i), -double(i)});
        const auto* front = length ? &split.view_data().front() : nullptr;
        const auto drained = split.drain_front(count);
        require(drained.size() == count && split.get_length() == length - count,
          "Drain sizes changed");
        if (count && count >= length - count && count != length / 2)
          require(&drained.front() == front, "Large-prefix drain copied the CPI");
        for (unsigned i = 0; i < count; ++i)
          require(drained[i] == std::complex<double>(i, -double(i)), "Drain reordered prefix");
        for (unsigned i = count; i < length; ++i)
          require(split.view_data()[i - count] == std::complex<double>(i, -double(i)),
            "Drain reordered retained tail");
      }
    }
    // Pointer/count bulk append must be identical to existing per-sample FIFO.
    for (unsigned capacity = 1; capacity <= 25; ++capacity) {
      for (unsigned initial = 0; initial <= capacity; ++initial) {
        for (unsigned count = 0; count <= 30; ++count) {
          IqData old(capacity), bulk(capacity);
          for (unsigned i = 0; i < initial; ++i) {
            old.push_back({double(i), double(i)}); bulk.push_back({double(i), double(i)});
          }
          std::vector<std::complex<float>> block(count);
          for (unsigned i = 0; i < count; ++i) {
            block[i] = {i + .25f, -float(i) - .5f}; old.push_back(std::complex<double>(block[i]));
          }
          bulk.append_unlocked(block.data(), block.size());
          require(old.view_data() == bulk.view_data(), "Bulk append differs from per-sample FIFO");
        }
      }
    }
    IqData zero(0);
    zero.append_unlocked(nullptr, 0);
    const std::complex<float> single{1, 2};
    zero.append_unlocked(&single, 1);
    require(zero.get_length() == 0, "Zero-capacity queue grew");
    bool nullRejected = false;
    try { zero.append_unlocked(nullptr, 1); }
    catch (const std::invalid_argument&) { nullRejected = true; }
    require(nullRejected, "Null positive-length append accepted");
    IqData data(5);
    data.discard_front(0);
    require(data.drain_front(0).empty(), "Empty drain returned samples");
    rejects([&] { data.discard_front(1); });
    for (int i = 0; i < 7; ++i) data.push_back({double(i), -double(i)});
    const auto* original = &data.view_data().front();
    auto whole = data.drain_front(5);
    require(whole.size() == 5 && &whole.front() == original,
      "Full drain copied the sample block");
    require(data.get_length() == 0 && whole.front().real() == 2,
      "Full drain changed FIFO order");
    data.replace(std::move(whole));
    auto prefix = data.drain_front(2);
    require(prefix.size() == 2 && prefix.front().real() == 2 &&
      prefix.back().real() == 3 && data.view_data().front().real() == 4,
      "Partial drain changed FIFO order");
    const auto remaining = data.get_data();
    rejects([&] { data.discard_front(4); });
    rejects([&] { data.drain_front(4); });
    require(data.view_data() == remaining, "Rejected drain mutated IQ");
    data.discard_front(1);
    require(data.get_length() == 2 && data.view_data().front().real() == 5,
      "Bulk discard changed retained tail");
    data.discard_front(2);
    require(data.get_length() == 0, "Full discard retained samples");
    data.replace({{1, 2}, {3, 4}, {5, 6}});
    const auto* writable = &data.view_data().front();
    auto& sameSize = data.resize_for_write(3);
    require(&sameSize.front() == writable && sameSize.size() == 3,
      "Same-size writable resize replaced sample storage");
    const auto beforeOversizedResize = data.get_data();
    rejects([&] { data.resize_for_write(6); });
    require(data.view_data() == beforeOversizedResize,
      "Oversized writable resize changed input");
    auto& resized = data.resize_for_write(5);
    resized[3] = {7, 8};
    resized[4] = {9, 10};
    require(data.get_length() == 5 && data.view_data()[4] == std::complex<double>(9, 10),
      "Writable resize did not expose the requested block");
    for (unsigned repeat = 0; repeat < 256; ++repeat) {
      data.append_unlocked({{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}, {11, 12}});
      require(data.get_length() == 5 && data.view_data().front().real() == 3,
        "Append lost newest-samples contract");
      data.clear();
    }
    data.replace({{100000000.25, 100000000.5}, {3, 4}, {5, 6}});
    const auto* owned = &data.view_data().front();
    const std::complex<float> estimate[] = {{100000000, 100000000}, {1, 2}};
    const auto unmodified = data.get_data();
    rejects([&] { data.subtract_clutter(nullptr, 2); });
    rejects([&] { data.subtract_clutter(estimate, 4); });
    require(data.view_data() == unmodified, "Rejected clutter estimate changed input");
    data.subtract_clutter(estimate, 2);
    require(&data.view_data().front() == owned && data.get_length() == 2,
      "Clutter subtraction copied the block or retained unfiltered samples");
    require(data.view_data()[0] == std::complex<double>(.25, .5) &&
      data.view_data()[1] == std::complex<double>(2, 2),
      "Clutter subtraction lost FP64 cancellation precision");
    data.replace({{100000000.25, 100000000.5}, {3, 4}, {5, 6}});
    const auto* doubleOwned = &data.view_data().front();
    const std::complex<double> doubleEstimate[] = {{200000000, 200000000}, {2, 4}};
    const auto doubleUnmodified = data.get_data();
    rejects([&] { data.subtract_clutter(nullptr, 2, 2); });
    rejects([&] { data.subtract_clutter(doubleEstimate, 4, 2); });
    rejects([&] { data.subtract_clutter(doubleEstimate, 2, 0); });
    require(data.view_data() == doubleUnmodified,
      "Rejected FP64 clutter estimate changed input");
    data.subtract_clutter(doubleEstimate, 2, 2);
    require(&data.view_data().front() == doubleOwned && data.get_length() == 2,
      "FP64 clutter subtraction copied the block or retained unfiltered samples");
    require(data.view_data()[0] == std::complex<double>(.25, .5) &&
      data.view_data()[1] == std::complex<double>(2, 2),
      "FP64 clutter subtraction changed the divide-before-subtract result");
    std::cout << "IQ FIFO ownership and retirement fixtures passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
