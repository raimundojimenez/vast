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

#include "vast/detail/radix_tree.hpp"

#include "vast/concept/parseable/core/parser.hpp"

namespace vast {

/// A dynamic parser which acts as an associative array. For symbols sharing
/// the same prefix, the parser returns the longest match.
template <class T>
struct symbol_table : parser<symbol_table<T>> {
  using attribute = T;

  symbol_table() = default;

  symbol_table(std::initializer_list<std::pair<const std::string, T>> init)
    : symbols(init) {
  }

  template <class Iterator, class Attribute>
  bool parse(Iterator& f, const Iterator& l, Attribute& a) const {
    auto input = std::string{f, l};
    auto prefixes = symbols.prefix_of(input);
    if (prefixes.empty())
      return false;
    auto longest_match = std::max_element(
      prefixes.begin(),
      prefixes.end(),
      [](auto x, auto y) { return x->first.size() < y->first.size(); }
    );
    a = (*longest_match)->second;
    std::advance(f, (*longest_match)->first.size());
    return true;
  }

  detail::radix_tree<T> symbols;
};

} // namespace vast

