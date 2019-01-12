// Copyright 2015-2018 Hans Dembinski
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_HISTOGRAM_DETAIL_LINEARIZE_HPP
#define BOOST_HISTOGRAM_DETAIL_LINEARIZE_HPP

#include <algorithm>
#include <boost/assert.hpp>
#include <boost/histogram/axis/traits.hpp>
#include <boost/histogram/axis/variant.hpp>
#include <boost/histogram/detail/axes.hpp>
#include <boost/histogram/detail/meta.hpp>
#include <boost/histogram/fwd.hpp>
#include <boost/histogram/unsafe_access.hpp>
#include <boost/mp11.hpp>
#include <boost/throw_exception.hpp>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#ifdef BOOST_HISTOGRAM_WITH_ACCUMULATORS_SUPPORT
#include <boost/accumulators/accumulators.hpp>
#endif

namespace boost {
namespace histogram {
namespace detail {

template <class T>
struct is_accumulator_set : std::false_type {};

#ifdef BOOST_HISTOGRAM_WITH_ACCUMULATORS_SUPPORT
template <class... Ts>
struct is_accumulator_set<::boost::accumulators::accumulator_set<Ts...>>
    : std::true_type {};
#endif

template <class T>
struct has_growing_axis_impl;

template <template <class, class...> class Container, class T, class... Us>
struct has_growing_axis_impl<Container<T, Us...>> {
  using type = has_method_update<T>;
};

template <template <class, class...> class Container, class... Ts, class... Us>
struct has_growing_axis_impl<Container<axis::variant<Ts...>, Us...>> {
  using type = mp11::mp_or<has_method_update<Ts>...>;
};

template <class... Ts>
struct has_growing_axis_impl<std::tuple<Ts...>> {
  using type = mp11::mp_or<has_method_update<Ts>...>;
};

template <class T>
using has_growing_axis = typename has_growing_axis_impl<T>::type;

/// Index with an invalid state
struct optional_index {
  std::size_t idx = 0;
  std::size_t stride = 1;
  operator bool() const { return stride > 0; }
  std::size_t operator*() const { return idx; }
};

inline void linearize(optional_index& out, const int axis_shape, int j) noexcept {
  // j is internal index, shifted by +1 wrt external index if axis has underflow bin
  out.idx += j * out.stride;
  // set stride to 0, if j is invalid
  out.stride *= (0 <= j && j < axis_shape) * axis_shape;
}

template <class A, class V>
void linearize_value(optional_index& out, int& shift, A& axis, const V& value) {
  int j;
  std::tie(j, shift) = axis::traits::update(axis, value);
  j += (axis::traits::options(axis) & axis::option_type::underflow);
  linearize(out, axis::traits::extend(axis), j);
}

template <class... Ts, class V>
void linearize_value(optional_index& o, int& s, axis::variant<Ts...>& a, const V& v) {
  axis::visit([&o, &s, &v](auto& a) { linearize_value(o, s, a, v); }, a);
}

template <class T>
void linearize_index(optional_index& out, const T& axis, const int j) {
  const auto extend = axis::traits::extend(axis);
  const auto opt = axis::traits::options(axis);
  linearize(out, extend, j + (opt & axis::option_type::underflow));
}

template <class S, class A, class T>
void maybe_replace_storage(S& storage, const A& axes, const T& shifts) {
  bool update_needed = false;
  for (int s : shifts) update_needed |= s != 0;
  if (!update_needed) return;
  struct item {
    int idx, size;
    std::size_t stride;
  };
  auto data = make_stack_buffer<item>(axes);
  auto sit = shifts.begin();
  auto dit = data.begin();
  std::size_t s = 1;
  for_each_axis(axes, [&](const auto& a) {
    const auto n = axis::traits::extend(a);
    *dit++ = {0, n, s};
    s *= n - std::abs(*sit++);
  });
  auto new_storage = make_default(storage);
  new_storage.reset(detail::bincount(axes));
  for (const auto& x : storage) {
    auto ns = new_storage.begin();
    sit = shifts.begin();
    for (const auto& d : data) { ns += (d.idx - std::min(*sit++, 0)) * d.stride; }
    auto dit = data.begin();
    const auto last = data.end() - 1;
    ++dit->idx;
    while (dit != last && dit->idx == dit->size) {
      dit->idx = 0;
      ++(++dit)->idx;
    }
    *ns = x;
  }
  storage = std::move(new_storage);
}

template <class T>
struct size_or_zero : mp11::mp_size_t<0> {};

template <class... Ts>
struct size_or_zero<std::tuple<Ts...>> : mp11::mp_size_t<sizeof...(Ts)> {};

// special case: if histogram::operator()(tuple(1, 2)) is called on 1d histogram with axis
// that accepts 2d tuple, this should not fail
// - solution is to forward tuples of size > 1 directly to axis for 1d histograms
// - has nice side-effect of making histogram::operator(1, 2) work as well
// - cannot detect call signature of axis at compile-time in all configurations
//   (axis::variant provides generic call interface and hides concrete interface),
//   so we throw at runtime if incompatible argument is passed (e.g. 3d tuple)
template <unsigned I, unsigned N, class S, class T, class U>
optional_index args_to_index(std::false_type, S&, T& axes, const U& args) {
  optional_index idx;
  int dummy;
  const auto rank = get_size(axes);
  if (rank == 1 && N > 1)
    linearize_value(idx, dummy, axis_get<0>(axes), tuple_slice<I, N>(args));
  else {
    if (rank != N)
      BOOST_THROW_EXCEPTION(
          std::invalid_argument("number of arguments != histogram rank"));
    constexpr unsigned M = size_or_zero<naked<decltype(axes)>>::value;
    mp11::mp_for_each<mp11::mp_iota_c<(M == 0 ? N : M)>>([&](auto J) {
      linearize_value(idx, dummy, axis_get<J>(axes), std::get<(J + I)>(args));
    });
  }
  return idx;
}

template <unsigned I, unsigned N, class S, class T, class U>
optional_index args_to_index(std::true_type, S& storage, T& axes, const U& args) {
  optional_index idx;
  auto shifts = make_stack_buffer<int>(axes, 0);
  const auto rank = get_size(axes);
  if (rank == 1 && N > 1)
    linearize_value(idx, shifts[0], axis_get<0>(axes), tuple_slice<I, N>(args));
  else {
    if (rank != N)
      BOOST_THROW_EXCEPTION(
          std::invalid_argument("number of arguments != histogram rank"));
    constexpr unsigned M = size_or_zero<naked<decltype(axes)>>::value;
    mp11::mp_for_each<mp11::mp_iota_c<(M == 0 ? N : M)>>([&](auto J) {
      linearize_value(idx, shifts[J], axis_get<J>(axes), std::get<(J + I)>(args));
    });
  }
  maybe_replace_storage(storage, axes, shifts);
  return idx;
}

template <typename U>
constexpr auto weight_sample_indices() {
  if (is_weight<U>::value) return std::make_pair(0, -1);
  if (is_sample<U>::value) return std::make_pair(-1, 0);
  return std::make_pair(-1, -1);
}

template <typename U0, typename U1, typename... Us>
constexpr auto weight_sample_indices() {
  using L = mp11::mp_list<U0, U1, Us...>;
  const int n = sizeof...(Us) + 1;
  if (is_weight<mp11::mp_at_c<L, 0>>::value) {
    if (is_sample<mp11::mp_at_c<L, 1>>::value) return std::make_pair(0, 1);
    if (is_sample<mp11::mp_at_c<L, n>>::value) return std::make_pair(0, n);
    return std::make_pair(0, -1);
  }
  if (is_sample<mp11::mp_at_c<L, 0>>::value) {
    if (is_weight<mp11::mp_at_c<L, 1>>::value) return std::make_pair(1, 0);
    if (is_weight<mp11::mp_at_c<L, n>>::value) return std::make_pair(n, 0);
    return std::make_pair(-1, 0);
  }
  if (is_weight<mp11::mp_at_c<L, n>>::value) {
    // 0, n already covered
    if (is_sample<mp11::mp_at_c<L, (n - 1)>>::value) return std::make_pair(n, n - 1);
    return std::make_pair(n, -1);
  }
  if (is_sample<mp11::mp_at_c<L, n>>::value) {
    // n, 0 already covered
    if (is_weight<mp11::mp_at_c<L, (n - 1)>>::value) return std::make_pair(n - 1, n);
    return std::make_pair(-1, n);
  }
  return std::make_pair(-1, -1);
}

template <class T, class U>
void fill_storage(mp11::mp_int<-1>, mp11::mp_int<-1>, T&& t, U&&) {
  static_if<is_incrementable<naked<T>>>([](auto&& t) { ++t; }, [](auto&& t) { t(); },
                                        std::forward<T>(t));
}

template <class IW, class T, class U>
void fill_storage(IW, mp11::mp_int<-1>, T&& t, U&& args) {
  static_if<is_incrementable<naked<T>>>(
      [](auto&& t, const auto& w) { t += w; },
      [](auto&& t, const auto& w) {
#ifdef BOOST_HISTOGRAM_WITH_ACCUMULATORS_SUPPORT
        static_if<is_accumulator_set<naked<T>>>(
            [w](auto&& t) { t(::boost::accumulators::weight = w); },
            [w](auto&& t) { t(w); }, t);
#else
        t(w);
#endif
      },
      std::forward<T>(t), std::get<IW::value>(args).value);
}

template <class IS, class T, class U>
void fill_storage(mp11::mp_int<-1>, IS, T&& t, U&& args) {
  mp11::tuple_apply([&t](auto&&... args) { t(args...); },
                    std::get<IS::value>(args).value);
}

template <class IW, class IS, class T, class U>
void fill_storage(IW, IS, T&& t, U&& args) {
#ifdef BOOST_HISTOGRAM_WITH_ACCUMULATORS_SUPPORT
  static_if<is_accumulator_set<naked<T>>>(
      [](auto&& t, const auto& w, const auto& s) {
        mp11::tuple_apply(
            [&](auto&&... args) { t(args..., ::boost::accumulators::weight = w); }, s);
      },
      [](auto&& t, const auto& w, const auto& s) {
        mp11::tuple_apply([&](auto&&... args) { t(w, args...); }, s);
      },
      std::forward<T>(t), std::get<IW::value>(args).value,
      std::get<IS::value>(args).value);
#else
  mp11::tuple_apply(
      [&](auto&&... args2) { t(std::get<IW::value>(args).value, args2...); },
      std::get<IS::value>(args).value);
#endif
}

template <class S, class A, class... Us>
void fill(S& storage, A& axes, const std::tuple<Us...>& args) {
  constexpr std::pair<int, int> iws = weight_sample_indices<Us...>();
  constexpr unsigned n = sizeof...(Us) - (iws.first > -1) - (iws.second > -1);
  constexpr unsigned i = (iws.first == 0 || iws.second == 0)
                             ? (iws.first == 1 || iws.second == 1 ? 2 : 1)
                             : 0;
  optional_index idx = args_to_index<i, n>(has_growing_axis<A>(), storage, axes, args);
  if (idx) {
    fill_storage(mp11::mp_int<iws.first>(), mp11::mp_int<iws.second>(), storage[*idx],
                 args);
  }
}

template <typename A, typename... Us>
optional_index at(const A& axes, const std::tuple<Us...>& args) {
  if (get_size(axes) != sizeof...(Us))
    BOOST_THROW_EXCEPTION(std::invalid_argument("number of arguments != histogram rank"));
  optional_index idx;
  mp11::mp_for_each<mp11::mp_iota_c<sizeof...(Us)>>([&](auto I) {
    linearize_index(idx, axis_get<I>(axes), static_cast<int>(std::get<I>(args)));
  });
  return idx;
}

template <typename A, typename U>
optional_index at(const A& axes, const U& args) {
  if (get_size(axes) != args.size())
    BOOST_THROW_EXCEPTION(std::invalid_argument("number of arguments != histogram rank"));
  optional_index idx;
  using std::begin;
  auto it = begin(args);
  for_each_axis(axes,
                [&](const auto& a) { linearize_index(idx, a, static_cast<int>(*it++)); });
  return idx;
}

} // namespace detail
} // namespace histogram
} // namespace boost

#endif
