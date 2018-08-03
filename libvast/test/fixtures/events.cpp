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

#include "fixtures/events.hpp"

#include "vast/const_table_slice_handle.hpp"
#include "vast/default_table_slice.hpp"
#include "vast/format/bgpdump.hpp"
#include "vast/format/bro.hpp"
#include "vast/format/test.hpp"
#include "vast/table_slice_builder.hpp"
#include "vast/table_slice_handle.hpp"
#include "vast/type.hpp"

#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/data.hpp"
#include "vast/concept/printable/vast/event.hpp"

namespace fixtures {

namespace {

timestamp epoch;

std::vector<event> make_ascending_integers(size_t count) {
  std::vector<event> result;
  type layout = type{record_type{{"value", integer_type{}}}}.name("test::int");
  for (size_t i = 0; i < count; ++i) {
    result.emplace_back(event::make(vector{static_cast<integer>(i)}, layout));
    result.back().timestamp(epoch + std::chrono::seconds(i));
  }
  return result;
}

std::vector<event> make_alternating_integers(size_t count) {
  std::vector<event> result;
  type layout = type{record_type{{"value", integer_type{}}}}.name("test::int");
  for (size_t i = 0; i < count; ++i) {
    result.emplace_back(event::make(vector{static_cast<integer>(i % 2)},
                                    layout));
    result.back().timestamp(epoch + std::chrono::seconds(i));
  }
  return result;
}

} // namespace <anonymous>

size_t events::slice_size = 100;

std::vector<event> events::bro_conn_log;
std::vector<event> events::bro_dns_log;
std::vector<event> events::bro_http_log;
std::vector<event> events::bgpdump_txt;
std::vector<event> events::random;

std::vector<table_slice_handle> events::bro_conn_log_slices;
std::vector<table_slice_handle> events::bro_dns_log_slices;
std::vector<table_slice_handle> events::bro_http_log_slices;
std::vector<table_slice_handle> events::bgpdump_txt_slices;
// std::vector<table_slice_handle> events::random_slices;

std::vector<const_table_slice_handle> events::const_bro_conn_log_slices;
// std::vector<const_table_slice_handle> events::const_bro_http_log_slices;
// std::vector<const_table_slice_handle> events::const_bro_dns_log_slices;
std::vector<const_table_slice_handle> events::const_bgpdump_txt_slices;
// std::vector<const_table_slice_handle> events::const_random_slices;

std::vector<event> events::ascending_integers;
std::vector<table_slice_handle> events::ascending_integers_slices;
std::vector<const_table_slice_handle> events::const_ascending_integers_slices;

std::vector<event> events::alternating_integers;
std::vector<table_slice_handle> events::alternating_integers_slices;
std::vector<const_table_slice_handle> events::const_alternating_integers_slices;

record_type events::bro_conn_log_layout() {
  return const_bro_conn_log_slices[0]->layout();
}

std::vector<table_slice_handle>
events::copy(std::vector<table_slice_handle> xs) {
  std::vector<table_slice_handle> result;
  result.reserve(xs.size());
  for (auto& x : xs)
    result.emplace_back(x->clone());
  return result;
}

class event_tracking_builder {
  table_slice_builder_ptr inner_;
  std::vector<event*> memory_;

public:
  event_tracking_builder() {
  }

  explicit event_tracking_builder(table_slice_builder_ptr b) : inner_{b} {
  }

  bool add(event* e) {
    if (!inner_->add(e->timestamp()))
      FAIL("builder->add() failed");
    if (!inner_->recursive_add(e->data(), e->type()))
      FAIL("builder->recursive_add() failed");

    memory_.push_back(e);
    return true;
  }

  auto rows() const {
    return inner_->rows();
  }

  const std::vector<event*> entries() const {
    return memory_;
  }

  table_slice_handle finish() {
    memory_.clear();
    return inner_->finish();
  }
};


/// Tries to access the builder for `layout`.
class builders {
  using Map = std::map<std::string, event_tracking_builder>;

  Map builders_;

public:
  event_tracking_builder* get(const type& layout) {
    auto i = builders_.find(layout.name());
    if (i != builders_.end())
      return &i->second;
    return caf::visit(
      detail::overload(
        [&](const record_type& rt) -> event_tracking_builder* {
          // We always add a timestamp as first column to the layout.
          auto internal = rt;
          record_field tstamp_field{"timestamp", timestamp_type{}};
          internal.fields.insert(internal.fields.begin(),
                                 std::move(tstamp_field));
          auto& ref = builders_[layout.name()];
          ref = event_tracking_builder{default_table_slice::make_builder(std::move(internal))};
          return &ref;
        },
        [&](auto&) -> event_tracking_builder* {
          FAIL("layout is not a record type");
          return nullptr;
        }),
      layout);
  }

  Map& all() {
    return builders_;
  }
};

events::events() {
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;
  MESSAGE("inhaling unit test suite events");
  bro_conn_log = inhale<format::bro::reader>(bro::conn);
  bro_dns_log = inhale<format::bro::reader>(bro::dns);
  bro_http_log = inhale<format::bro::reader>(bro::http);
  bgpdump_txt = inhale<format::bgpdump::reader>(bgpdump::updates20140821);
  random = extract(vast::format::test::reader{42, 1000});
  ascending_integers = make_ascending_integers(10000);
  alternating_integers = make_alternating_integers(10000);
  auto allocate_id_block = [i = id{0}](size_t size) mutable {
    auto first = i;
    i += size;
    return first;
  };
  MESSAGE("building slices of " << slice_size << " events each");
  auto slice_up = [&](std::vector<event>& src) {
    VAST_ASSERT(src.size() > 0);
    VAST_ASSERT(caf::holds_alternative<record_type>(src[0].type()));
    std::vector<table_slice_handle> slices;
    builders bs;
    auto finish_slice = [&](auto& builder) {
      auto first = allocate_id_block(slice_size);
      /// inject ids into the events here
      size_t i = 0;
      for(auto eptr : builder.entries()) {
        eptr->id(first + i++);
      }
      slices.emplace_back(builder.finish());
      slices.back()->offset(first);
    };
    for (auto& e : src) {
      auto bptr = bs.get(e.type());
      bptr->add(&e);
      if (bptr->rows() == slice_size)
        finish_slice(*bptr);
    }
    for (auto& [_, builder] : bs.all()) {
      if (builder.rows() > 0)
        finish_slice(builder);
    }
    return slices;
  };
  bro_conn_log_slices = slice_up(bro_conn_log);
  bro_dns_log_slices = slice_up(bro_dns_log);
  allocate_id_block(1000); // cause an artificial gap in the ID sequence
  bro_http_log_slices = slice_up(bro_http_log);
  bgpdump_txt_slices = slice_up(bgpdump_txt);
  //random_slices = slice_up(random);
  ascending_integers_slices = slice_up(ascending_integers);
  alternating_integers_slices = slice_up(alternating_integers);
  auto to_const_vector = [](const auto& xs) {
    std::vector<const_table_slice_handle> result;
    result.reserve(xs.size());
    result.insert(result.begin(), xs.begin(), xs.end());
    return result;
  };
  const_bro_conn_log_slices = to_const_vector(bro_conn_log_slices);
  // const_bro_dns_log_slices = to_const_vector(bro_dns_log_slices);
  // const_bro_http_log_slices = to_const_vector(bro_http_log_slices);
  const_bgpdump_txt_slices = to_const_vector(bgpdump_txt_slices);
  // const_random_slices = to_const_vector(random_slices);
  const_ascending_integers_slices = to_const_vector(ascending_integers_slices);
  const_alternating_integers_slices
    = to_const_vector(alternating_integers_slices);
  auto sort_by_id = [](std::vector<event>& v) {
    std::sort(
      v.begin(), v.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.id() < rhs.id(); });
  };
  auto to_events = [&](const auto& slices) {
    std::vector<event> result;
    for (auto& slice : slices) {
      auto xs = slice->rows_to_events();
      std::move(xs.begin(), xs.end(), std::back_inserter(result));
    }
    sort_by_id(result);
    return result;
  };
#define SANITY_CHECK(event_vec, slice_vec)                                     \
  {                                                                            \
    auto flat_log = to_events(slice_vec);                                      \
    auto sorted_event_vec = event_vec;                                         \
    sort_by_id(sorted_event_vec);                                              \
    REQUIRE_EQUAL(sorted_event_vec.size(), flat_log.size());                   \
    for (size_t i = 0; i < sorted_event_vec.size(); ++i) {                     \
      if (flatten(sorted_event_vec[i]) != flat_log[i]) {                       \
        FAIL(#event_vec << " != " << #slice_vec << "\ni: " << i << '\n'        \
                        << to_string(sorted_event_vec[i])                      \
                        << " != " << to_string(flat_log[i]));                  \
      }                                                                        \
    }                                                                          \
  }
  SANITY_CHECK(bro_conn_log, const_bro_conn_log_slices);
  //SANITY_CHECK(bro_dns_log, const_bro_dns_log_slices);
  //SANITY_CHECK(bro_http_log, const_bro_http_log_slices);
  SANITY_CHECK(bgpdump_txt, const_bgpdump_txt_slices);
  //SANITY_CHECK(random, const_random_slices);
}

} // namespace fixtures
