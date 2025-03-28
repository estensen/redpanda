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
#include "cluster/controller_stm.h"
#include "cluster/errc.h"
#include "cluster/fwd.h"
#include "cluster/logger.h"
#include "cluster/types.h"
#include "config/node_config.h"
#include "config/tls_config.h"
#include "net/dns.h"
#include "outcome_future_utils.h"
#include "rpc/connection_cache.h"
#include "rpc/types.h"

#include <seastar/core/sharded.hh>

#include <system_error>
#include <utility>

namespace detail {

template<typename T, typename Fn>
std::vector<cluster::topic_result>
create_topic_results(const std::vector<T>& topics, Fn fn) {
    std::vector<cluster::topic_result> results;
    results.reserve(topics.size());
    std::transform(
      topics.cbegin(),
      topics.cend(),
      std::back_inserter(results),
      [&fn](const T& t) { return fn(t); });
    return results;
}

} // namespace detail

namespace config {
struct configuration;
}

namespace cluster {

class metadata_cache;
/// This method calculates the machine nodes that were updated/added
/// and removed
patch<broker_ptr> calculate_changed_brokers(
  const std::vector<broker_ptr>& new_list,
  const std::vector<broker_ptr>& old_list);

/// Creates the same topic_result for all requests
template<typename T>
requires requires(const T& req) {
    { req.tp_ns } -> std::convertible_to<const model::topic_namespace&>;
}
std::vector<topic_result>
create_topic_results(const std::vector<T>& requests, errc error_code) {
    return detail::create_topic_results(requests, [error_code](const T& r) {
        return topic_result(r.tp_ns, error_code);
    });
}

inline std::vector<topic_result> create_topic_results(
  const std::vector<model::topic_namespace>& topics, errc error_code) {
    return detail::create_topic_results(
      topics, [error_code](const model::topic_namespace& t) {
          return topic_result(t, error_code);
      });
}

inline std::vector<topic_result> create_topic_results(
  const std::vector<custom_assignable_topic_configuration>& requests,
  errc error_code) {
    return detail::create_topic_results(
      requests, [error_code](const custom_assignable_topic_configuration& r) {
          return topic_result(r.cfg.tp_ns, error_code);
      });
}

inline std::vector<topic_result> create_topic_results(
  const std::vector<non_replicable_topic>& requests, errc error_code) {
    return detail::create_topic_results(
      requests, [error_code](const non_replicable_topic& nrt) {
          return topic_result(nrt.name, error_code);
      });
}

ss::future<> update_broker_client(
  model::node_id,
  ss::sharded<rpc::connection_cache>&,
  model::node_id node,
  net::unresolved_address addr,
  config::tls_config);

ss::future<> remove_broker_client(
  model::node_id, ss::sharded<rpc::connection_cache>&, model::node_id);

template<typename Proto, typename Func>
requires requires(Func&& f, Proto c) { f(c); }
auto with_client(
  model::node_id self,
  ss::sharded<rpc::connection_cache>& cache,
  model::node_id id,
  net::unresolved_address addr,
  config::tls_config tls_config,
  rpc::clock_type::duration connection_timeout,
  Func&& f) {
    return update_broker_client(
             self, cache, id, std::move(addr), std::move(tls_config))
      .then([id,
             self,
             &cache,
             f = std::forward<Func>(f),
             connection_timeout]() mutable {
          return cache.local().with_node_client<Proto, Func>(
            self,
            ss::this_shard_id(),
            id,
            connection_timeout,
            std::forward<Func>(f));
      });
}

/// Creates current broker instance using its configuration.
model::broker make_self_broker(const config::node_config& node_cfg);

/// \brief Log reload credential event
/// The function is supposed to be invoked from the callback passed to
/// 'build_reloadable_*_credentials' methods.
///
/// \param log is a ss::logger instance that should be used
/// \param system_name is a name of the subsystem that uses credentials
/// \param updated is a set of updated credential names
/// \param eptr is an exception ptr in case of error
void log_certificate_reload_event(
  ss::logger& log,
  const char* system_name,
  const std::unordered_set<ss::sstring>& updated,
  const std::exception_ptr& eptr);

inline ss::future<ss::shared_ptr<ss::tls::certificate_credentials>>
maybe_build_reloadable_certificate_credentials(config::tls_config tls_config) {
    return std::move(tls_config)
      .get_credentials_builder()
      .then([](std::optional<ss::tls::credentials_builder> credentials) {
          if (credentials) {
              return credentials->build_reloadable_certificate_credentials(
                [](
                  const std::unordered_set<ss::sstring>& updated,
                  const std::exception_ptr& eptr) {
                    log_certificate_reload_event(
                      clusterlog, "Client TLS", updated, eptr);
                });
          }
          return ss::make_ready_future<
            ss::shared_ptr<ss::tls::certificate_credentials>>(nullptr);
      });
}

template<typename Proto, typename Func>
requires requires(Func&& f, Proto c) { f(c); }
auto do_with_client_one_shot(
  net::unresolved_address addr,
  config::tls_config tls_config,
  rpc::clock_type::duration connection_timeout,
  Func&& f) {
    return maybe_build_reloadable_certificate_credentials(std::move(tls_config))
      .then(
        [f = std::forward<Func>(f), connection_timeout, addr = std::move(addr)](
          ss::shared_ptr<ss::tls::certificate_credentials>&& cert) mutable {
            auto transport = ss::make_lw_shared<rpc::transport>(
              rpc::transport_configuration{
                .server_addr = std::move(addr),
                .credentials = std::move(cert),
                .disable_metrics = net::metrics_disabled(true)});

            return transport->connect(connection_timeout)
              .then([transport, f = std::forward<Func>(f)]() mutable {
                  return ss::futurize_invoke(
                    std::forward<Func>(f), Proto(*transport));
              })
              .finally([transport] {
                  transport->shutdown();
                  return transport->stop().finally([transport] {});
              });
        });
}

/**
 * checks if current node/shard is part of the partition replica set replica set
 */
bool has_local_replicas(
  model::node_id, const std::vector<model::broker_shard>&);

bool are_replica_sets_equal(
  const std::vector<model::broker_shard>&,
  const std::vector<model::broker_shard>&);

template<typename Cmd>
ss::future<std::error_code> replicate_and_wait(
  ss::sharded<controller_stm>& stm,
  ss::sharded<feature_table>& feature_table,
  ss::sharded<ss::abort_source>& as,
  Cmd&& cmd,
  model::timeout_clock::time_point timeout,
  std::optional<model::term_id> term = std::nullopt) {
    const bool use_serde_serialization = feature_table.local().is_active(
      feature::serde_raft_0);
    return stm.invoke_on(
      controller_stm_shard,
      [cmd = std::forward<Cmd>(cmd),
       term,
       &as = as,
       timeout,
       use_serde_serialization](controller_stm& stm) mutable {
          if (likely(use_serde_serialization)) {
              auto b = serde_serialize_cmd(std::forward<Cmd>(cmd));
              return stm.replicate_and_wait(
                std::move(b), timeout, as.local(), term);
          }
          return serialize_cmd(std::forward<Cmd>(cmd))
            .then([&stm, timeout, term, &as](model::record_batch b) {
                return stm.replicate_and_wait(
                  std::move(b), timeout, as.local(), term);
            });
      });
}

std::vector<custom_assignable_topic_configuration>
  without_custom_assignments(std::vector<topic_configuration>);

inline bool has_non_replicable_op_type(const topic_table_delta& d) {
    using op_t = topic_table_delta::op_type;
    switch (d.type) {
    case op_t::add_non_replicable:
    case op_t::del_non_replicable:
        return true;
    case op_t::add:
    case op_t::del:
    case op_t::update:
    case op_t::update_finished:
    case op_t::update_properties:
    case op_t::cancel_update:
    case op_t::force_abort_update:
        return false;
    }
    __builtin_unreachable();
}
/**
 * Subtracts second replica set from the first one. Result contains only brokers
 * shards that are present in first replica set but not in the second one.
 */
inline std::vector<model::broker_shard> subtract_replica_sets(
  const std::vector<model::broker_shard>& lhs,
  const std::vector<model::broker_shard>& rhs) {
    std::vector<model::broker_shard> ret;
    std::copy_if(
      lhs.begin(),
      lhs.end(),
      std::back_inserter(ret),
      [&rhs](const model::broker_shard& bs) {
          return std::find(rhs.begin(), rhs.end(), bs) == rhs.end();
      });
    return ret;
}

/**
 * Subtracts second replica set from the first one. Result contains only brokers
 * that node_ids are present in the first list but not the other one
 */
inline std::vector<model::broker_shard> subtract_replica_sets_by_node_id(
  const std::vector<model::broker_shard>& lhs,
  const std::vector<model::broker_shard>& rhs) {
    std::vector<model::broker_shard> ret;
    std::copy_if(
      lhs.begin(),
      lhs.end(),
      std::back_inserter(ret),
      [&rhs](const model::broker_shard& lhs_bs) {
          return std::find_if(
                   rhs.begin(),
                   rhs.end(),
                   [&lhs_bs](const model::broker_shard& rhs_bs) {
                       return rhs_bs.node_id == lhs_bs.node_id;
                   })
                 == rhs.end();
      });
    return ret;
}
// check if replica set contains a node
inline bool contains_node(
  const std::vector<model::broker_shard>& replicas, model::node_id id) {
    return std::find_if(
             replicas.begin(),
             replicas.end(),
             [id](const model::broker_shard& bs) { return bs.node_id == id; })
           != replicas.end();
}

cluster::errc map_update_interruption_error_code(std::error_code);

} // namespace cluster
