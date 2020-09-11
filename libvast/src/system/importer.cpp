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

#include "vast/system/importer.hpp"

#include "vast/concept/printable/numeric/integral.hpp"
#include "vast/concept/printable/std/chrono.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/error.hpp"
#include "vast/concept/printable/vast/filesystem.hpp"
#include "vast/defaults.hpp"
#include "vast/detail/fill_status_map.hpp"
#include "vast/fwd.hpp"
#include "vast/logger.hpp"
#include "vast/si_literals.hpp"
#include "vast/system/report.hpp"
#include "vast/system/type_registry.hpp"
#include "vast/table_slice.hpp"

#include <caf/atom.hpp>
#include <caf/attach_continuous_stream_stage.hpp>
#include <caf/config_value.hpp>
#include <caf/settings.hpp>

#include <fstream>

namespace vast::system {

importer_state::importer_state(caf::event_based_actor* self_ptr)
  : self(self_ptr) {
  // nop
}

importer_state::~importer_state() {
  write_state(write_mode::with_next);
}

caf::error importer_state::read_state() {
  auto file = dir / "current_id_block";
  if (exists(file)) {
    VAST_VERBOSE(self, "reads persistent state from", file);
    std::ifstream state_file{to_string(file)};
    state_file >> current.end;
    if (!state_file)
      return make_error(ec::parse_error, "unable to read importer state file",
                        file.str());
    state_file >> current.next;
    if (!state_file) {
      VAST_WARNING(self, "did not find next ID position in state file; "
                         "irregular shutdown detected");
      current.next = current.end;
    }
  } else {
    VAST_VERBOSE(self, "did not find a state file at", file);
    current.end = 0;
    current.next = 0;
  }
  return get_next_block();
}

caf::error importer_state::write_state(write_mode mode) {
  if (!exists(dir)) {
    if (auto err = mkdir(dir))
      return err;
  }
  std::ofstream state_file{to_string(dir / "current_id_block")};
  state_file << current.end;
  if (mode == write_mode::with_next) {
    state_file << " " << current.next;
    VAST_VERBOSE(self, "persisted ID block [", current.next, ",", current.end,
                 ")");
  } else {
    VAST_VERBOSE(self, "persisted ID block boundary at", current.end);
  }
  return caf::none;
}

caf::error importer_state::get_next_block(uint64_t required) {
  using namespace si_literals;
  while (current.next + required >= current.end)
    current.end += 8_Mi;
  return write_state(write_mode::without_next);
}

id importer_state::next_id(uint64_t advance) {
  id pre = current.next;
  id post = pre + advance;
  if (post >= current.end)
    get_next_block(advance);
  current.next = post;
  VAST_ASSERT(current.next < current.end);
  return pre;
}

id importer_state::available_ids() const noexcept {
  return max_id - current.next;
}

caf::dictionary<caf::config_value>
importer_state::status(status_verbosity v) const {
  auto result = caf::settings{};
  auto& importer_status = put_dictionary(result, "importer");
  // TODO: caf::config_value can only represent signed 64 bit integers, which
  // may make it look like overflow happened in the status report. As an
  // intermediate workaround, we convert the values to strings.
  if (v >= status_verbosity::detailed) {
    caf::put(importer_status, "ids.available", to_string(available_ids()));
    caf::put(importer_status, "ids.block.next", to_string(current.next));
    caf::put(importer_status, "ids.block.end", to_string(current.end));
  }
  // General state such as open streams.
  if (v >= status_verbosity::debug)
    detail::fill_status_map(importer_status, self);
  return result;
}

void importer_state::send_report() {
  auto now = stopwatch::now();
  if (measurement_.events > 0) {
    using namespace std::string_literals;
    auto elapsed = std::chrono::duration_cast<duration>(now - last_report);
    auto node_throughput = measurement{elapsed, measurement_.events};
    auto r = performance_report{
      {{"importer"s, measurement_}, {"node_throughput"s, node_throughput}}};
#if VAST_LOG_LEVEL >= VAST_LOG_LEVEL_VERBOSE
    auto beat = [&](const auto& sample) {
      if (auto rate = sample.value.rate_per_sec(); std::isfinite(rate))
        VAST_VERBOSE(self, "handled", sample.value.events,
                     "events at a rate of", static_cast<uint64_t>(rate),
                     "events/sec in", to_string(sample.value.duration));
      else
        VAST_VERBOSE(self, "handled", sample.value.events, "events in",
                     to_string(sample.value.duration));
    };
    beat(r[1]);
#endif
    measurement_ = measurement{};
    self->send(accountant, std::move(r));
  }
  last_report = now;
}

caf::behavior importer(importer_actor* self, path dir, archive_type archive,
                       caf::actor index, type_registry_type type_registry) {
  VAST_TRACE(VAST_ARG(dir));
  self->state.dir = dir;
  self->state.congested_batches = 0;
  auto err = self->state.read_state();
  if (err) {
    VAST_ERROR(self, "failed to load state:", self->system().render(err));
    self->quit(std::move(err));
    return {};
  }
  namespace defs = defaults::system;
  self->set_exit_handler([=](const caf::exit_msg& msg) {
    self->state.send_report();
    self->quit(msg.reason);
  });
  self->state.stg = caf::attach_continuous_stream_stage(
    self,
    [](caf::unit_t&) {
      // nop
    },
    [=](caf::unit_t&, caf::downstream<table_slice_ptr>& out,
        table_slice_ptr x) {
      VAST_TRACE(VAST_ARG(x));
      auto& st = self->state;
      auto t = timer::start(st.measurement_);
      VAST_ASSERT(x->rows() <= static_cast<size_t>(st.available_ids()));
      auto events = x->rows();
      x.unshared().offset(st.next_id(events));
#if VAST_LOG_LEVEL >= CAF_LOG_LEVEL_DEBUG
      // We don't want to report minimal congestions, so we're setting a minimum
      // reporting threshold here.
      constexpr auto congestion_reporting_threshold = size_t{100};
      auto is_congested = [&] {
        for (const auto& mgr : self->stream_managers())
          if (mgr.second->congested())
            return true;
        return false;
      };
      if (is_congested()) {
        if (st.congested_batches == congestion_reporting_threshold)
          VAST_DEBUG(self, "is currently congested downstream");
        ++st.congested_batches;
      } else {
        if (st.congested_batches > congestion_reporting_threshold)
          VAST_DEBUG(self, "resolved congestion of", st.congested_batches,
                     "table slices");
        st.congested_batches = 0;
      }
#endif
      out.push(std::move(x));
      t.stop(events);
    },
    [=](caf::unit_t&, const error& err) {
      VAST_DEBUG(self, "stopped with message:", err);
    });
  if (type_registry)
    self->state.stg->add_outbound_path(type_registry);
  if (archive)
    self->state.stg->add_outbound_path(archive);
  if (index) {
    self->state.index = index;
    self->state.stg->add_outbound_path(index);
  }
  return {
    [=](accountant_type accountant) {
      VAST_DEBUG(self, "registers accountant", archive);
      self->state.accountant = std::move(accountant);
      self->send(self->state.accountant, atom::announce_v, self->name());
      self->delayed_send(self, defs::telemetry_rate, atom::telemetry_v);
      self->state.last_report = stopwatch::now();
    },
    [=](atom::exporter, const caf::actor& exporter) {
      VAST_DEBUG(self, "registers exporter", exporter);
      return self->state.stg->add_outbound_path(exporter);
    },
    [=](caf::stream<importer_state::input_type>& in) {
      auto& st = self->state;
      VAST_DEBUG(self, "adds a new source:", self->current_sender());
      st.stg->add_inbound_path(in);
    },
    [=](atom::add, const caf::actor& subscriber) {
      auto& st = self->state;
      VAST_DEBUG(self, "adds a new sink:", self->current_sender());
      st.stg->add_outbound_path(subscriber);
    },
    [=](atom::subscribe, atom::flush, caf::actor& listener) {
      VAST_ASSERT(self->state.stg != nullptr);
      self->send(index, atom::subscribe_v, atom::flush_v, listener);
    },
    [=](atom::status, status_verbosity v) { return self->state.status(v); },
    [=](atom::telemetry) {
      self->state.send_report();
      self->delayed_send(self, defs::telemetry_rate, atom::telemetry_v);
    },
  };
}

} // namespace vast::system
