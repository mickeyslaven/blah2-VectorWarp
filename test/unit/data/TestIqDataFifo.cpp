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
    std::cout << "IQ FIFO ownership and retirement fixtures passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
