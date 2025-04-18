/*
 * Copyright 2020 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once
#include "cluster/commands.h"
#include "cluster/scheduling/partition_allocator.h"
#include "cluster/topic_table.h"
#include "cluster/types.h"
#include "model/fundamental.h"
#include "model/record.h"

#include <seastar/core/sharded.hh>

namespace cluster {

// The topic updates dispatcher is responsible for receiving update_apply
// upcalls from controller state machine and propagating updates to topic state
// core local copies. The dispatcher handles partition_allocator updates. The
// partition allocator exists only on core 0 hence the updates have to be
// executed at the same core.
//
//
//                                  +----------------+        +------------+
//                              +-->| Table@core_0   |---+    |            |
//                              |   +----------------+   |    |  Allocator |
//                              |                        |    |            |
//                              |   +----------------+   |    +------------+
//                              +-->| Table@core_0   |---+          ^
//    on core 0                 |   +----------------+   |          |
//   +-----+   +------------+   |                        |    +------------+
//   |     |   |            |   |           .            |    |            |
//   | STM +-->+ Dispatcher +-->+           .            +--->+ Dispatcher +--->
//   |     |   |            |   |           .            |    |            |
//   +-----+   +------------+   |                        |    +------------+
//                              |   +----------------+   |   on core 0
//                              +-->| Table@core n-1 |---+
//                              |   +----------------+   |
//                              |                        |
//                              |   +----------------+   |
//                              +-->| Table@core #n  |---+
//                                  +----------------+
//
class topic_updates_dispatcher {
public:
    topic_updates_dispatcher(
      ss::sharded<partition_allocator>&,
      ss::sharded<topic_table>&,
      ss::sharded<partition_leaders_table>&);

    ss::future<std::error_code> apply_update(model::record_batch);

    static constexpr auto commands = make_commands_list<
      create_topic_cmd,
      delete_topic_cmd,
      move_partition_replicas_cmd,
      finish_moving_partition_replicas_cmd,
      update_topic_properties_cmd,
      create_partition_cmd,
      create_non_replicable_topic_cmd,
      cancel_moving_partition_replicas_cmd>();

    bool is_batch_applicable(const model::record_batch& batch) const {
        return batch.header().type
               == model::record_batch_type::topic_management_cmd;
    }

private:
    using in_progress_map = absl::
      node_hash_map<model::partition_id, std::vector<model::broker_shard>>;
    template<typename Cmd>
    ss::future<std::error_code> dispatch_updates_to_cores(Cmd, model::offset);

    using ntp_leader = std::pair<model::ntp, model::node_id>;

    ss::future<> update_leaders_with_estimates(std::vector<ntp_leader> leaders);
    void update_allocations(std::vector<partition_assignment>);

    void deallocate_topic(const assignments_set&, const in_progress_map&);

    in_progress_map
    collect_in_progress(const model::topic_namespace&, const assignments_set&);

    ss::sharded<partition_allocator>& _partition_allocator;
    ss::sharded<topic_table>& _topic_table;
    ss::sharded<partition_leaders_table>& _partition_leaders_table;
};

} // namespace cluster
