/*
 * Copyright 2021 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */
#pragma once
#include "cluster/node/types.h"
#include "config/property.h"
#include "storage/types.h"
#include "units.h"

#include <seastar/core/sstring.hh>

#include <sys/statvfs.h>

#include <chrono>
#include <functional>

// Local node state monitoring is kept separate in case we want to access it
// pre-quorum formation in the future, etc.
namespace cluster::node {

class local_monitor {
public:
    local_monitor(
      config::binding<size_t> min_bytes_alert,
      config::binding<unsigned> min_percent_alert,
      config::binding<size_t> min_bytes,
      ss::sharded<storage::node_api>&,
      ss::sharded<storage::api>&);
    local_monitor(const local_monitor&) = delete;
    local_monitor(local_monitor&&) = default;
    ~local_monitor() = default;
    local_monitor& operator=(local_monitor const&) = delete;
    local_monitor& operator=(local_monitor&&) = delete;

    ss::future<> update_state();
    const local_state& get_state_cached() const;
    static storage::disk_space_alert
    eval_disks(const std::vector<storage::disk>&);

    static constexpr std::string_view stable_alert_string
      = "storage space alert"; // for those who grep the logs..

    void testing_only_set_path(const ss::sstring& path);
    void testing_only_set_statvfs(
      std::function<struct statvfs(const ss::sstring)>);

private:
    // helpers
    static size_t
    alert_percent_in_bytes(unsigned alert_percent, size_t bytes_available);

    ss::future<std::vector<storage::disk>> get_disks();
    ss::future<struct statvfs> get_statvfs(const ss::sstring);
    void update_alert_state();
    ss::future<> update_disk_metrics();
    float percent_free(const storage::disk& disk);
    void maybe_log_space_error(
      const storage::disk&, const storage::disk_space_alert);

    // configuration
    void refresh_configuration();
    std::size_t get_config_alert_threshold_bytes();
    unsigned get_config_alert_threshold_percent();

    // state
    local_state _state;
    ss::logger::rate_limit _despam_interval = ss::logger::rate_limit(
      std::chrono::hours(1));
    config::binding<size_t> _free_bytes_alert_threshold;
    config::binding<unsigned> _free_percent_alert_threshold;
    config::binding<size_t> _min_free_bytes;

    ss::sharded<storage::node_api>& _storage_node_api; // single instance
    ss::sharded<storage::api>& _storage_api;

    // Injection points for unit tests
    ss::sstring _path_for_test;
    std::function<struct statvfs(const ss::sstring)> _statvfs_for_test;
};

} // namespace cluster::node