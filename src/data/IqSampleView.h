// Nonowning FP64 reader for canonical DSP and legacy FIFO storage. The owner
// must remain unchanged through the synchronous operation (including workers).
#pragma once
#include <algorithm>
#include <complex>
#include <cstddef>
#include <deque>
#include <iterator>
#include <limits>
#include <stdexcept>

class IqSampleView {
public:
  using value_type = std::complex<double>;
  class const_iterator {
    const std::complex<double>* contiguous_ = nullptr;
    const std::deque<std::complex<double>>* deque_ = nullptr;
    std::ptrdiff_t position_ = 0;
  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = std::complex<double>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;
    const_iterator() = default;
    const_iterator(pointer contiguous, const std::deque<value_type>* deque,
        difference_type position) : contiguous_(contiguous), deque_(deque), position_(position) {}
    reference operator*() const { return deque_ ? (*deque_)[position_] : contiguous_[position_]; }
    pointer operator->() const { return &**this; }
    reference operator[](difference_type n) const { return *(*this + n); }
    const_iterator& operator++() { ++position_; return *this; }
    const_iterator operator++(int) { auto previous = *this; ++*this; return previous; }
    const_iterator& operator--() { --position_; return *this; }
    const_iterator operator--(int) { auto previous = *this; --*this; return previous; }
    const_iterator& operator+=(difference_type n) { position_ += n; return *this; }
    const_iterator& operator-=(difference_type n) { position_ -= n; return *this; }
    friend const_iterator operator+(const_iterator value, difference_type n) { return value += n; }
    friend const_iterator operator+(difference_type n, const_iterator value) { return value += n; }
    friend const_iterator operator-(const_iterator value, difference_type n) { return value -= n; }
    friend difference_type operator-(const_iterator a, const_iterator b) { return a.position_ - b.position_; }
    friend bool operator==(const_iterator a, const_iterator b) {
      return a.position_ == b.position_ && a.contiguous_ == b.contiguous_ && a.deque_ == b.deque_;
    }
    friend bool operator!=(const_iterator a, const_iterator b) { return !(a == b); }
    friend bool operator<(const_iterator a, const_iterator b) { return a.position_ < b.position_; }
    friend bool operator>(const_iterator a, const_iterator b) { return b < a; }
    friend bool operator<=(const_iterator a, const_iterator b) { return !(b < a); }
    friend bool operator>=(const_iterator a, const_iterator b) { return !(a < b); }
  };
  IqSampleView() = default;
  IqSampleView(const std::deque<value_type>& samples) : deque_(&samples), count_(samples.size()) {}
  IqSampleView(const value_type* samples, std::size_t count) : contiguous_(samples), count_(count) {
    if (!samples && count) throw std::invalid_argument("Null contiguous IQ view");
    if (count > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(value_type))
      throw std::overflow_error("Contiguous IQ view is too large");
  }
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  const value_type* data() const { return contiguous_; }
  const value_type& operator[](std::size_t index) const { return deque_ ? (*deque_)[index] : contiguous_[index]; }
  const value_type& at(std::size_t index) const {
    if (index >= count_) throw std::out_of_range("IQ view sample index");
    return (*this)[index];
  }
  const_iterator begin() const { return {contiguous_, deque_, 0}; }
  const_iterator end() const { return {contiguous_, deque_, std::ptrdiff_t(count_)}; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  template<class Output> void copy_to(Output destination, std::size_t count, std::size_t first = 0) const {
    if (first > count_ || count > count_ - first) throw std::out_of_range("IQ view copy range");
    if (!count) return;
    if (deque_) std::copy_n(deque_->begin() + first, count, destination);
    else std::copy_n(contiguous_ + first, count, destination);
  }
private:
  const value_type* contiguous_ = nullptr;
  const std::deque<value_type>* deque_ = nullptr;
  std::size_t count_ = 0;
};
