// Copyright 2015-2018 Hans Dembinski
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_HISTOGRAM_HISTOGRAM_HPP
#define BOOST_HISTOGRAM_HISTOGRAM_HPP

#include <algorithm>
#include <boost/assert.hpp>
#include <boost/histogram/arithmetic_operators.hpp>
#include <boost/histogram/detail/axes.hpp>
#include <boost/histogram/detail/index_mapper.hpp>
#include <boost/histogram/detail/meta.hpp>
#include <boost/histogram/histogram_fwd.hpp>
#include <boost/histogram/iterator.hpp>
#include <boost/mp11.hpp>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace boost {
namespace histogram {

template <typename Axes, typename Storage>
class histogram {
  static_assert(mp11::mp_size<Axes>::value > 0, "at least one axis required");

public:
  using axes_type = Axes;
  using storage_type = Storage;
  using value_type = typename storage_type::value_type;
  using const_reference = typename storage_type::const_reference;
  using const_iterator = iterator<histogram>;

  histogram() = default;
  histogram(const histogram& rhs) = default;
  histogram(histogram&& rhs) = default;
  histogram& operator=(const histogram& rhs) = default;
  histogram& operator=(histogram&& rhs) = default;

  template <typename A, typename S>
  explicit histogram(const histogram<A, S>& rhs) : storage_(rhs.storage_) {
    detail::axes_assign(axes_, rhs.axes_);
  }

  template <typename A, typename S>
  histogram& operator=(const histogram<A, S>& rhs) {
    storage_ = rhs.storage_;
    detail::axes_assign(axes_, rhs.axes_);
    return *this;
  }

  explicit histogram(const axes_type& a, storage_type s = {})
      : axes_(a), storage_(std::move(s)) {
    storage_.reset(detail::bincount(axes_));
  }

  explicit histogram(axes_type&& a, storage_type s = {})
      : axes_(std::move(a)), storage_(std::move(s)) {
    storage_.reset(detail::bincount(axes_));
  }

  template <typename A, typename S>
  bool operator==(const histogram<A, S>& rhs) const noexcept {
    return detail::axes_equal(axes_, rhs.axes_) && storage_ == rhs.storage_;
  }

  template <typename A, typename S>
  bool operator!=(const histogram<A, S>& rhs) const noexcept {
    return !operator==(rhs);
  }

  template <typename A, typename S>
  histogram& operator+=(const histogram<A, S>& rhs) {
    if (!detail::axes_equal(axes_, rhs.axes_))
      throw std::invalid_argument("axes of histograms differ");
    storage_ += rhs.storage_;
    return *this;
  }

  histogram& operator*=(const double x) {
    storage_ *= x;
    return *this;
  }

  histogram& operator/=(const double x) {
    storage_ *= 1.0 / x;
    return *this;
  }

  /// Number of axes (dimensions) of histogram
  std::size_t rank() const noexcept { return detail::axes_size(axes_); }

  /// Total number of bins in the histogram (including underflow/overflow)
  std::size_t size() const noexcept { return storage_.size(); }

  /// Reset bin counters to zero
  void reset() { storage_.reset(storage_.size()); }

  /// Get N-th axis (const version)
  template <std::size_t N>
  decltype(auto) axis(mp11::mp_size_t<N>) const {
    detail::range_check(axes_, N);
    return detail::axis_get<N>(axes_);
  }

  /// Get N-th axis
  template <std::size_t N>
  decltype(auto) axis(mp11::mp_size_t<N>) {
    detail::range_check(axes_, N);
    return detail::axis_get<N>(axes_);
  }

  /// Get first axis (convenience for 1-d histograms, const version)
  decltype(auto) axis() const { return axis(mp11::mp_size_t<0>()); }

  /// Get first axis (convenience for 1-d histograms)
  decltype(auto) axis() { return axis(mp11::mp_size_t<0>()); }

  /// Get N-th axis with runtime index (const version)
  decltype(auto) axis(std::size_t i) const {
    detail::range_check(axes_, i);
    return detail::axis_get(axes_, i);
  }

  /// Get N-th axis with runtime index
  decltype(auto) axis(std::size_t i) {
    detail::range_check(axes_, i);
    return detail::axis_get(axes_, i);
  }

  /// Apply unary functor/function to each axis
  template <typename Unary>
  void for_each_axis(Unary&& unary) const {
    detail::for_each_axis(axes_, std::forward<Unary>(unary));
  }

  /// Fill histogram with value tuple and optional weight
  template <typename... Ts>
  void operator()(const Ts&... ts) {
    // case with one argument needs special treatment, specialized below
    operator()(std::forward_as_tuple(ts...));
  }

  /// Fill histogram with value tuple and optional weight
  template <typename... Ts>
  void operator()(const std::tuple<Ts...>& t) {
    detail::fill_impl(storage_, axes_, t);
  }

  /// Access bin counter at indices
  template <typename... Ts>
  const_reference at(const Ts&... ts) const {
    // case with one argument is ambiguous, is specialized below
    return at(std::forward_as_tuple(ts...));
  }

  /// Access bin counter at index (specialization for 1D)
  template <typename... Ts>
  const_reference at(const std::tuple<Ts...>& t) const {
    const auto idx = detail::at_impl(axes_, t);
    if (!idx) throw std::out_of_range("indices out of bounds");
    return storage_[*idx];
  }

  /// Access bin counter at index
  template <typename T>
  const_reference operator[](const T& t) const {
    return at(t);
  }

  /// Returns a lower-dimensional histogram
  // precondition: argument sequence must be strictly ascending axis indices
  template <std::size_t I, typename... Ns>
  auto reduce_to(mp11::mp_size_t<I>, Ns...) const
      -> histogram<detail::sub_axes<axes_type, mp11::mp_size_t<I>, Ns...>, storage_type> {
    using N = mp11::mp_size_t<I>;
    using LN = mp11::mp_list<N, Ns...>;
    detail::range_check(axes_, detail::mp_last<LN>::value);
    using sub_axes_type = detail::sub_axes<axes_type, N, Ns...>;
    using HR = histogram<sub_axes_type, storage_type>;
    auto sub_axes = detail::make_sub_axes(axes_, N(), Ns()...);
    auto hr = HR(std::move(sub_axes),
                 detail::static_if<detail::has_allocator<storage_type>>(
                     [this](auto) { return storage_type(storage_.get_allocator()); },
                     [](auto) { return storage_type(); }, 0));
    const auto b = detail::bool_mask<N, Ns...>(rank(), true);
    std::vector<unsigned> shape(rank());
    for_each_axis(detail::shape_collector(shape.begin()));
    detail::index_mapper m(shape, b);
    do { hr.storage_.add(m.second, storage_[m.first]); } while (m.next());
    return hr;
  }

  /// Returns a lower-dimensional histogram
  // precondition: sequence must be strictly ascending axis indices
  template <typename Iterator, typename U = axes_type,
            typename = detail::requires_axis_vector<U>,
            typename = detail::requires_iterator<Iterator>>
  histogram reduce_to(Iterator begin, Iterator end) const {
    BOOST_ASSERT_MSG(std::is_sorted(begin, end, std::less_equal<decltype(*begin)>()),
                     "integer sequence must be strictly ascending");
    BOOST_ASSERT_MSG(begin == end || static_cast<unsigned>(*(end - 1)) < rank(),
                     "index out of range");
    auto sub_axes = histogram::axes_type(axes_.get_allocator());
    sub_axes.reserve(std::distance(begin, end));
    auto b = std::vector<bool>(rank(), false);
    for (auto it = begin; it != end; ++it) {
      sub_axes.push_back(axes_[*it]);
      b[*it] = true;
    }
    auto hr = histogram(std::move(sub_axes), storage_type(storage_.get_allocator()));
    std::vector<unsigned> shape(rank());
    for_each_axis(detail::shape_collector(shape.begin()));
    detail::index_mapper m(shape, b);
    do { hr.storage_.add(m.second, storage_[m.first]); } while (m.next());
    return hr;
  }

  auto begin() const noexcept { return const_iterator(*this, 0); }

  auto end() const noexcept { return const_iterator(*this, size()); }

  template <typename Archive>
  void serialize(Archive&, unsigned);

private:
  axes_type axes_;
  storage_type storage_;

  template <typename A, typename S>
  friend class histogram;
  template <typename H>
  friend class iterator;
  friend class unsafe_access;
};

} // namespace histogram
} // namespace boost

#endif
