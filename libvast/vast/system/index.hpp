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

#include "vast/detail/flat_lru_cache.hpp"
#include "vast/detail/lru_cache.hpp"
#include "vast/detail/stable_map.hpp"
#include "vast/expression.hpp"
#include "vast/fbs/index.hpp"
#include "vast/fwd.hpp"
#include "vast/meta_index.hpp"
#include "vast/status.hpp"
#include "vast/system/accountant.hpp"
#include "vast/system/filesystem.hpp"
#include "vast/system/indexer_stage_driver.hpp"
#include "vast/system/partition.hpp"
#include "vast/system/query_supervisor.hpp"
#include "vast/uuid.hpp"

#include <caf/actor.hpp>
#include <caf/behavior.hpp>
#include <caf/fwd.hpp>
#include <caf/meta/omittable_if_empty.hpp>
#include <caf/meta/type_name.hpp>

#include <unordered_map>
#include <vector>

#include "caf/response_promise.hpp"

namespace vast::system {

/// The state of the active partition.
struct active_partition_info {
  /// The partition actor.
  caf::actor actor;

  /// The slot ID that identifies the partition in the stream.
  caf::stream_slot stream_slot;

  /// The remaining free capacity of the partition.
  size_t capacity;

  /// The UUID of the partition.
  uuid id;

  template <class Inspector>
  friend auto inspect(Inspector& f, active_partition_info& x) {
    return f(caf::meta::type_name("active_partition_info"), x.actor,
             x.stream_slot, x.capacity, x.id);
  }
};

/// Accumulates statistics for a given layout.
struct layout_statistics {
  uint64_t count; ///< Number of events indexed.

  template <class Inspector>
  friend auto inspect(Inspector& f, layout_statistics& x) {
    return f(caf::meta::type_name("layout_statistics"), x.count);
  }
};

/// Accumulates statistics about indexed data.
struct index_statistics {
  /// The number of events for a given layout.
  std::unordered_map<std::string, layout_statistics> layouts;

  template <class Inspector>
  friend auto inspect(Inspector& f, index_statistics& x) {
    return f(caf::meta::type_name("index_statistics"), x.layouts);
  }
};

/// Loads partitions from disk by UUID.
class partition_factory {
public:
  explicit partition_factory(index_state& state);

  filesystem_type& fs(); // getter/setter

  caf::actor operator()(const uuid& id) const;

private:
  filesystem_type fs_;
  const index_state& state_;
};

using pending_query_map = detail::stable_map<uuid, evaluation_triples>;

struct query_state {
  /// The UUID of the query.
  vast::uuid id;

  /// The query expression.
  vast::expression expression;

  /// Unscheduled partitions.
  std::vector<uuid> partitions;

  template <class Inspector>
  friend auto inspect(Inspector& f, query_state& x) {
    return f(caf::meta::type_name("query_state"), x.id, x.expression,
             caf::meta::omittable_if_empty(), x.partitions);
  }
};

/// The state of the index actor.
struct index_state {
  // -- type aliases -----------------------------------------------------------

  using index_stream_stage_ptr
    = caf::stream_stage_ptr<table_slice_ptr,
                            caf::broadcast_downstream_manager<table_slice_ptr>>;

  // -- constructor ------------------------------------------------------------

  explicit index_state(caf::stateful_actor<index_state>* self);

  // -- persistence ------------------------------------------------------------

  caf::error load_from_disk();

  /// @returns various status metrics.
  caf::dictionary<caf::config_value> status(status_verbosity v) const;

  void flush_to_disk();

  path index_filename(path basename = {}) const;

  // -- query handling

  bool worker_available();

  caf::actor next_worker();

  /// Get the actor handles for up to `num_partitions` PARTITION actors,
  /// spawning them if needed.
  std::vector<std::pair<uuid, caf::actor>>
  collect_query_actors(query_state& lookup, uint32_t num_partitions);

  /// Spawns one evaluator for each partition.
  /// @returns a query map for passing to INDEX workers over the spawned
  ///          EVALUATOR actors.
  query_map launch_evaluators(pending_query_map& pqm, expression expr);

  // -- flush handling ---------------------------------------------------

  /// Adds a new flush listener.
  void add_flush_listener(caf::actor listener);

  /// Sends a notification to all listeners and clears the listeners list.
  void notify_flush_listeners();

  // -- data members ----------------------------------------------------------

  /// Pointer to the parent actor.
  caf::stateful_actor<index_state>* self;

  /// The streaming stage.
  index_stream_stage_ptr stage;

  /// Allows the index to multiplex between waiting for ready workers and
  /// queries.
  caf::behavior has_worker;

  /// The single active (read/write) partition.
  active_partition_info active_partition = {};

  /// Partitions that are currently in the process of persisting.
  // TODO: An alternative to keeping an explicit set of unpersisted partitions
  // would be to add functionality to the LRU cache to "pin" certain items.
  // Then (assuming the query interface for both types of partition stays
  // identical) we could just use the same cache for unpersisted partitions and
  // unpin them after they're safely on disk.
  std::unordered_map<uuid, caf::actor> unpersisted;

  /// The set of passive (read-only) partitions currently loaded into memory.
  /// Uses the `partition_factory` to load new partitions as needed, and evicts
  /// old entries when the size exceeds `max_inmem_partitions`.
  detail::lru_cache<uuid, caf::actor, partition_factory> inmem_partitions;

  /// The set of partitions that exist on disk.
  std::unordered_set<uuid> persisted_partitions;

  /// The maximum number of events that a partition can hold.
  size_t partition_capacity;

  // The maximum size of the partition LRU cache (or the maximum number of
  // read-only partition loaded to memory).
  size_t max_inmem_partitions;

  // The number of partitions initially returned for a query.
  size_t taste_partitions;

  /// Maps query IDs to pending lookup state.
  std::unordered_map<uuid, query_state> pending;

  /// Caches idle workers.
  std::vector<caf::actor> idle_workers;

  /// The meta index.
  meta_index meta_idx;

  /// The directory for persistent state.
  path dir;

  /// Statistics about processed data.
  index_statistics stats;

  // Handle of the accountant.
  accountant_type accountant;

  /// List of actors that wait for the next flush event.
  std::vector<caf::actor> flush_listeners;

  /// Disables regular persisting of global state.
  //  TODO: This is a workaround for situations where the meta index becomes
  //  big enough that writing it becomes a significant performance issue for
  //  the indexer. Ideally, we want to move the meta index state into the
  //  individual partitions, so this would become irrelevant.
  bool delay_flush_until_shutdown;

  /// Actor handle of the filesystem actor.
  filesystem_type filesystem;

  /// Actor handle to the node.
  caf::weak_actor_ptr node;

  static inline const char* name = "index";
};

/// Flatbuffer integration. Note that this is only one-way, restoring
/// the index state needs additional runtime information.
// TODO: Pull out the persisted part of the state into a separate struct
// that can be packed and unpacked.
caf::expected<flatbuffers::Offset<fbs::Index>>
pack(flatbuffers::FlatBufferBuilder& builder, const index_state& x);

/// Indexes events in horizontal partitions.
/// @param fs The filesystem actor. Not used by the index itself but forwarded
/// to partitions.
/// @param dir The directory of the index.
/// @param partition_capacity The maximum number of events per partition.
/// @pre `partition_capacity > 0
caf::behavior index(caf::stateful_actor<index_state>* self, filesystem_type fs,
                    path dir, size_t partition_capacity,
                    size_t in_mem_partitions, size_t taste_partitions,
                    size_t num_workers, bool delay_flush_until_shutdown);

} // namespace vast::system
