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

#include "vast/format/syslog.hpp"

#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/time.hpp"
#include "vast/error.hpp"
#include "vast/fwd.hpp"
#include "vast/type.hpp"

#include <caf/expected.hpp>
#include <caf/settings.hpp>

namespace vast {
namespace format {
namespace syslog {

namespace {

type make_rfc5424_type() {
  // clang-format off
  return record_type{{
    {"facility", count_type{}},
    {"severity", count_type{}},
    {"version", count_type{}},
    {"ts", time_type{}.attributes({{"timestamp"}})},
    {"hostname", string_type{}},
    {"app_name", string_type{}},
    {"process_id", string_type{}},
    {"message_id", string_type{}},
    // TODO: The index is currently incapable of handling map_type. Hence, the
    // structured_data is disabled.
    // {"structered_data", map_type{
    //   string_type{}.name("id"),
    //   map_type{string_type{}.name("key"), string_type{}.name("value")}.name("params")},
    // },
    {"message", string_type{}},
  }}.name("syslog.rfc5424");
  // clang-format on
}

type make_unknown_type() {
  // clang-format off
  return record_type{{
    {"syslog_message", string_type{}}
  }}.name("syslog.unknown");
  // clang-format on
}

} // namespace

reader::reader(caf::atom_value table_slice_type, const caf::settings& options,
               std::unique_ptr<std::istream> in)
  : super(table_slice_type),
    syslog_rfc5424_type_{make_rfc5424_type()},
    syslog_unkown_type_{make_unknown_type()} {
  if (auto read_timeout_arg = caf::get_if<std::string>(&options, "import.batch-"
                                                                 "timeout")) {
    if (auto read_timeout = to<decltype(read_timeout_)>(*read_timeout_arg))
      read_timeout_ = *read_timeout;
    else
      VAST_WARNING(this, "cannot set import.batch-timeout to",
                   *read_timeout_arg, "as it is not a valid duration");
  }
  if (in != nullptr)
    reset(std::move(in));
}

caf::error reader::schema(vast::schema x) {
  // clang-format off
  return replace_if_congruent({
    &syslog_rfc5424_type_,
    &syslog_unkown_type_
  }, x);
  // clang-format on
}

vast::schema reader::schema() const {
  vast::schema sch;
  sch.add(syslog_rfc5424_type_);
  sch.add(syslog_unkown_type_);
  return sch;
}

void reader::reset(std::unique_ptr<std::istream> in) {
  VAST_ASSERT(in != nullptr);
  input_ = std::move(in);
  lines_ = std::make_unique<detail::line_range>(*input_);
}

const char* reader::name() const {
  return "syslog-reader";
}

caf::error
reader::read_impl(size_t max_events, size_t max_slice_size, consumer& f) {
  table_slice_builder_ptr bptr = nullptr;
  auto next_line = [&, start = std::chrono::steady_clock::now()] {
    auto remaining = start + read_timeout_ - std::chrono::steady_clock::now();
    if (remaining < std::chrono::steady_clock::duration::zero())
      return true;
    if (!bptr || bptr->rows() == 0) {
      lines_->next();
      return false;
    }
    return lines_->next_timeout(remaining);
  };
  for (size_t produced = 0; produced < max_events;) {
    if (lines_->done())
      return finish(f, make_error(ec::end_of_input, "input exhausted"));
    auto timeout = next_line();
    // We must check not only for a timeout but also whether any events were
    // produced to work around CAF's assumption that sources are always able to
    // generate events. Once `caf::stream_source` can handle empty batches
    // gracefully, the second check should be removed.
    if (timeout && produced > 0) {
      VAST_DEBUG(this, "reached input timeout at line", lines_->line_number());
      return finish(f, ec::timeout);
    }
    auto& line = lines_->get();
    if (line.empty()) {
      // Ignore empty lines.
      VAST_DEBUG(this, "ignores empty line at", lines_->line_number());
      continue;
    }
    message sys_msg;
    auto parser = message_parser{};
    if (parser(line, sys_msg)) {
      bptr = builder(syslog_rfc5424_type_);
      if (!bptr) {
        return finish(f, make_error(ec::format_error,
                                    "failed to get create table "
                                    "slice builder for type "
                                      + syslog_rfc5424_type_.name()));
      }
      // TODO: The index is currently incapable of handling map_type. Hence, the
      // structured_data is disabled.
      if (!bptr->add(sys_msg.hdr.facility, sys_msg.hdr.severity,
                     sys_msg.hdr.version, sys_msg.hdr.ts, sys_msg.hdr.hostname,
                     sys_msg.hdr.app_name, sys_msg.hdr.process_id,
                     sys_msg.hdr.msg_id,
                     /*sys_msg.data,*/ sys_msg.msg))
        return finish(f, make_error(ec::format_error,
                                    "failed to produce table slice row for "
                                      + bptr->layout().name()));
      if (bptr->rows() >= max_slice_size)
        if (auto err = finish(f))
          return err;
    } else {
      bptr = builder(syslog_unkown_type_);
      if (!bptr)
        return finish(f, make_error(ec::format_error,
                                    "failed to get create table "
                                    "slice builder for type "
                                      + syslog_unkown_type_.name()));
      if (!bptr->add(line))
        return finish(f, make_error(ec::format_error,
                                    "failed to produce table slice row for "
                                      + bptr->layout().name()));
      if (bptr->rows() >= max_slice_size)
        if (auto err = finish(f))
          return err;
    }
    ++produced;
  }
  return caf::none;
}

} // namespace syslog
} // namespace format
} // namespace vast
