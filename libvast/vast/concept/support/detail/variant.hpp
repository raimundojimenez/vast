/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#pragma once

#include <caf/variant.hpp>
#include <caf/detail/type_list.hpp>

#include "vast/detail/type_list.hpp"

namespace vast::detail {

template <class... Ts>
struct lazy_type_list {
  using type = type_list<Ts...>;
};

template <class T, class U>
struct lazy_variant_concat {
  using type = tl_concat_t<
    tl_make_t<typename T::types>,
    tl_make_t<typename U::types>
  >;
};

template <class T, class U>
struct lazy_variant_push_back {
  using type = tl_push_back_t<
    tl_make_t<typename T::types>,
    U
  >;
};

template <class T, class U>
using variant_type_concat =
  tl_distinct_t<
    typename std::conditional_t<
      caf::is_variant<T>{} && caf::is_variant<U>{},
      lazy_variant_concat<T, U>,
      std::conditional_t<
        caf::is_variant<T>{},
        lazy_variant_push_back<T, U>,
        std::conditional_t<
          caf::is_variant<U>{},
          lazy_variant_push_back<U, T>,
          lazy_type_list<T, U>
        >
      >
    >::type
  >;


template <class T, class U>
using flattened_variant = caf::detail::tl_apply_t<
  variant_type_concat<T, U>,
  caf::variant
>;

} // namespace vast::detail
