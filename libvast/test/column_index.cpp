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

#define SUITE column_index

#include "vast/column_index.hpp"

#include "vast/test/fixtures/actor_system_and_events.hpp"
#include "vast/test/test.hpp"

#include "vast/caf_table_slice.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/expression.hpp"
#include "vast/table_slice_builder.hpp"
#include "vast/table_slice_column.hpp"
#include "vast/type.hpp"

using namespace vast;

namespace {

struct fixture : fixtures::deterministic_actor_system_and_events {
  fixture() {
    directory /= "column-index";
  }

  auto lookup(column_index_ptr& idx, const curried_predicate& pred) {
    return unbox(idx->lookup(pred.op, make_view(pred.rhs)));
  }
};

} // namespace <anonymous>

FIXTURE_SCOPE(column_index_tests, fixture)

TEST(skip attribute) {
  auto foo_type = integer_type{}.name("foo");
  auto bar_type = integer_type{}.attributes({{"skip"}}).name("bar");
  auto foo
    = unbox(make_column_index(sys, directory, foo_type, caf::settings{}));
  auto bar
    = unbox(make_column_index(sys, directory, bar_type, caf::settings{}));
  CHECK_EQUAL(foo->has_skip_attribute(), false);
  CHECK_EQUAL(bar->has_skip_attribute(), true);
}

TEST(integer values) {
  MESSAGE("ingest integer values");
  integer_type column_type;
  record_type layout{{"value", column_type}};
  auto col
    = unbox(make_column_index(sys, directory, column_type, caf::settings{}));
  auto rows = make_rows(1, 2, 3, 1, 2, 3, 1, 2, 3);
  auto slice = caf_table_slice::make(layout, rows);
  auto column = table_slice_column{slice, 0};
  col->add(column);
  REQUIRE_EQUAL(slice->rows(), rows.size());
  auto slice_size = rows.size();
  MESSAGE("generate test queries");
  auto is1 = curried(unbox(to<predicate>(":int == +1")));
  auto is2 = curried(unbox(to<predicate>(":int == +2")));
  auto is3 = curried(unbox(to<predicate>(":int == +3")));
  auto is4 = curried(unbox(to<predicate>(":int == +4")));
  MESSAGE("verify column index");
  CHECK_EQUAL(lookup(col, is1), make_ids({0, 3, 6}, slice_size));
  CHECK_EQUAL(lookup(col, is2), make_ids({1, 4, 7}, slice_size));
  CHECK_EQUAL(lookup(col, is3), make_ids({2, 5, 8}, slice_size));
  CHECK_EQUAL(lookup(col, is4), make_ids({}, slice_size));
  MESSAGE("persist and reload from disk");
  col->flush_to_disk();
  col.reset();
  col = unbox(make_column_index(sys, directory, column_type, caf::settings{}));
  MESSAGE("verify column index again");
  CHECK_EQUAL(lookup(col, is1), make_ids({0, 3, 6}, slice_size));
  CHECK_EQUAL(lookup(col, is2), make_ids({1, 4, 7}, slice_size));
  CHECK_EQUAL(lookup(col, is3), make_ids({2, 5, 8}, slice_size));
  CHECK_EQUAL(lookup(col, is4), make_ids({}, slice_size));
}

TEST(zeek conn log) {
  MESSAGE("ingest originators from zeek conn log");
  auto& layout = zeek_conn_log[0]->layout();
  auto col_offset = unbox(layout.resolve("id.orig_h"));
  auto col_type = layout.at(col_offset);
  auto col_index = unbox(layout.flat_index_at(col_offset));
  REQUIRE_EQUAL(col_index, 2u); // 3rd column
  auto col
    = unbox(make_column_index(sys, directory, *col_type, caf::settings{}));
  for (auto slice : zeek_conn_log)
    col->add({slice, col_index});
  MESSAGE("verify column index");
  auto pred = curried(unbox(to<predicate>(":addr == 192.168.1.103")));
  auto expected_result = make_ids({1, 3, 7, 14, 16}, rows(zeek_conn_log));
  CHECK_EQUAL(lookup(col, pred), expected_result);
  MESSAGE("persist and reload from disk");
  col->flush_to_disk();
  col.reset();
  MESSAGE("verify column index again");
  col = unbox(make_column_index(sys, directory, *col_type, caf::settings{}));
  CHECK_EQUAL(lookup(col, pred), expected_result);
}

FIXTURE_SCOPE_END()
