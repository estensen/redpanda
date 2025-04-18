// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "net/server.h"

#include "config/configuration.h"
#include "likely.h"
#include "prometheus/prometheus_sanitize.h"
#include "rpc/logger.h"
#include "rpc/service.h"
#include "seastar/core/coroutine.hh"
#include "ssx/future-util.h"
#include "ssx/metrics.h"
#include "ssx/sformat.h"
#include "vassert.h"
#include "vlog.h"

#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/net/api.hh>
#include <seastar/util/later.hh>

namespace net {

server::server(server_configuration c)
  : cfg(std::move(c))
  , _memory(cfg.max_service_memory_per_core)
  , _public_metrics(ssx::metrics::public_metrics_handle) {}

server::server(ss::sharded<server_configuration>* s)
  : server(s->local()) {}

server::~server() = default;

void server::start() {
    vassert(_proto, "must have a registered protocol before starting");
    if (!cfg.disable_metrics) {
        setup_metrics();
        _probe.setup_metrics(_metrics, cfg.name.c_str());
    }

    if (!cfg.disable_public_metrics) {
        setup_public_metrics();
        _probe.setup_public_metrics(_public_metrics, cfg.name.c_str());
    }

    if (cfg.connection_rate_bindings) {
        connection_rate_bindings.emplace(cfg.connection_rate_bindings.value());

        connection_rate_info info{
          .max_connection_rate
          = connection_rate_bindings.value().config_general_rate(),
          .overrides
          = connection_rate_bindings.value().config_overrides_rate()};

        _connection_rates.emplace(std::move(info), _conn_gate, _probe);

        connection_rate_bindings.value().config_general_rate.watch([this] {
            _connection_rates->update_general_rate(
              connection_rate_bindings.value().config_general_rate());
        });
        connection_rate_bindings.value().config_overrides_rate.watch([this] {
            _connection_rates->update_overrides_rate(
              connection_rate_bindings.value().config_overrides_rate());
        });
    }
    for (const auto& endpoint : cfg.addrs) {
        ss::server_socket ss;
        try {
            ss::listen_options lo;
            lo.reuse_address = true;
            lo.lba = cfg.load_balancing_algo;
            if (cfg.listen_backlog.has_value()) {
                lo.listen_backlog = cfg.listen_backlog.value();
            }

            if (!endpoint.credentials) {
                ss = ss::engine().listen(endpoint.addr, lo);
            } else {
                ss = ss::tls::listen(
                  endpoint.credentials, ss::engine().listen(endpoint.addr, lo));
            }
        } catch (...) {
            throw std::runtime_error(fmt::format(
              "{} - Error attempting to listen on {}: {}",
              _proto->name(),
              endpoint,
              std::current_exception()));
        }
        auto& b = _listeners.emplace_back(
          std::make_unique<listener>(endpoint.name, std::move(ss)));
        listener& ref = *b;
        // background
        ssx::spawn_with_gate(_conn_gate, [this, &ref] { return accept(ref); });
    }
}

static inline void print_exceptional_future(
  server::protocol* proto,
  ss::future<> f,
  const char* ctx,
  ss::socket_address address) {
    if (likely(!f.failed())) {
        f.ignore_ready_future();
        return;
    }

    auto ex = f.get_exception();
    auto disconnected = is_disconnect_exception(ex);

    if (!disconnected) {
        vlog(
          rpc::rpclog.error,
          "{} - Error[{}] remote address: {} - {}",
          proto->name(),
          ctx,
          address,
          ex);
    } else {
        vlog(
          rpc::rpclog.info,
          "{} - Disconnected {} ({}, {})",
          proto->name(),
          address,
          ctx,
          disconnected.value());
    }
}

static ss::future<> apply_proto(
  server::protocol* proto, server::resources&& rs, conn_quota::units cq_units) {
    auto conn = rs.conn;
    return proto->apply(std::move(rs))
      .then_wrapped(
        [proto, conn, cq_units = std::move(cq_units)](ss::future<> f) {
            print_exceptional_future(
              proto, std::move(f), "applying protocol", conn->addr);
            return conn->shutdown().then_wrapped(
              [proto, addr = conn->addr](ss::future<> f) {
                  print_exceptional_future(
                    proto, std::move(f), "shutting down", addr);
              });
        })
      .finally([conn] {});
}

ss::future<> server::accept(listener& s) {
    return ss::repeat([this, &s]() mutable {
        return s.socket.accept().then_wrapped(
          [this, &s](ss::future<ss::accept_result> f_cs_sa) mutable
          -> ss::future<ss::stop_iteration> {
              if (_as.abort_requested()) {
                  f_cs_sa.ignore_ready_future();
                  co_return ss::stop_iteration::yes;
              }
              auto ar = f_cs_sa.get();
              ar.connection.set_nodelay(true);
              ar.connection.set_keepalive(true);

              // `s` is a lambda reference argument to a coroutine:
              // this is the last place we may refer to it before
              // any scheduling points.
              auto name = s.name;

              conn_quota::units cq_units;
              if (cfg.conn_quotas) {
                  cq_units = co_await cfg.conn_quotas->get().local().get(
                    ar.remote_address.addr());
                  if (!cq_units.live()) {
                      // Connection limit hit, drop this connection.
                      _probe.connection_rejected();
                      vlog(
                        rpc::rpclog.info,
                        "Connection limit reached, rejecting {}",
                        ar.remote_address.addr());
                      co_return ss::stop_iteration::no;
                  }
              }

              // Apply socket buffer size settings
              if (cfg.tcp_recv_buf.has_value()) {
                  // Explicitly store in an int to decouple the
                  // config type from the set_sockopt type.
                  int recv_buf = cfg.tcp_recv_buf.value();
                  ar.connection.set_sockopt(
                    SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf));
              }

              if (cfg.tcp_send_buf.has_value()) {
                  int send_buf = cfg.tcp_send_buf.value();
                  ar.connection.set_sockopt(
                    SOL_SOCKET, SO_SNDBUF, &send_buf, sizeof(send_buf));
              }

              if (_connection_rates) {
                  try {
                      co_await _connection_rates->maybe_wait(
                        ar.remote_address.addr());
                  } catch (const std::exception& e) {
                      vlog(
                        rpc::rpclog.trace,
                        "Timeout while waiting free token for connection rate. "
                        "addr:{}",
                        ar.remote_address);
                      _probe.timeout_waiting_rate_limit();
                      co_return ss::stop_iteration::no;
                  }
              }

              auto conn = ss::make_lw_shared<net::connection>(
                _connections,
                name,
                std::move(ar.connection),
                ar.remote_address,
                _probe,
                cfg.stream_recv_buf);
              vlog(
                rpc::rpclog.trace,
                "{} - Incoming connection from {} on \"{}\"",
                _proto->name(),
                ar.remote_address,
                name);
              if (_conn_gate.is_closed()) {
                  co_await conn->shutdown();
                  throw ss::gate_closed_exception();
              }
              ssx::spawn_with_gate(
                _conn_gate,
                [this, conn, cq_units = std::move(cq_units)]() mutable {
                    return apply_proto(
                      _proto.get(), resources(this, conn), std::move(cq_units));
                });
              co_return ss::stop_iteration::no;
          });
    });
}

void server::shutdown_input() {
    ss::sstring proto_name = _proto ? _proto->name() : "protocol not set";
    vlog(
      rpc::rpclog.info,
      "{} - Stopping {} listeners",
      proto_name,
      _listeners.size());
    for (auto& l : _listeners) {
        l->socket.abort_accept();
    }
    vlog(rpc::rpclog.debug, "{} - Service probes {}", proto_name, _probe);
    vlog(
      rpc::rpclog.info,
      "{} - Shutting down {} connections",
      proto_name,
      _connections.size());
    _as.request_abort();
    // close the connections and wait for all dispatches to finish
    for (auto& c : _connections) {
        c.shutdown_input();
    }
}

ss::future<> server::wait_for_shutdown() {
    if (!_as.abort_requested()) {
        shutdown_input();
    }

    if (_connection_rates.has_value()) {
        _connection_rates->stop();
    }

    return _conn_gate.close().then([this] {
        return seastar::do_for_each(
          _connections, [](net::connection& c) { return c.shutdown(); });
    });
}

ss::future<> server::stop() {
    // if shutdown input was already requested this method is nop, user has to
    // wait explicitly for shutdown to finish with `wait_for_shutdown`
    if (_as.abort_requested()) {
        return ss::now();
    }
    // if shutdown_input wasn't called fallback to previous behavior i.e. stop()
    // waits for shutdown
    return wait_for_shutdown();
}

void server::setup_metrics() {
    namespace sm = ss::metrics;
    if (!_proto) {
        return;
    }
    _metrics.add_group(
      prometheus_sanitize::metrics_name(cfg.name),
      {sm::make_total_bytes(
         "max_service_mem_bytes",
         [this] { return cfg.max_service_memory_per_core; },
         sm::description(
           ssx::sformat("{}: Maximum memory allowed for RPC", cfg.name))),
       sm::make_total_bytes(
         "consumed_mem_bytes",
         [this] { return cfg.max_service_memory_per_core - _memory.current(); },
         sm::description(ssx::sformat(
           "{}: Memory consumed by request processing", cfg.name))),
       sm::make_histogram(
         "dispatch_handler_latency",
         [this] { return _hist.seastar_histogram_logform(); },
         sm::description(ssx::sformat("{}: Latency ", cfg.name)))});
}

void server::setup_public_metrics() {
    namespace sm = ss::metrics;
    if (!_proto) {
        return;
    }

    std::string_view server_name(cfg.name);

    if (server_name.ends_with("_rpc")) {
        server_name.remove_suffix(4);
    }

    auto server_label = ssx::metrics::make_namespaced_label("server");

    _public_metrics.add_group(
      prometheus_sanitize::metrics_name("rpc:request"),
      {sm::make_histogram(
         "latency_seconds",
         sm::description("RPC latency"),
         {server_label(server_name)},
         [this] { return ssx::metrics::report_default_histogram(_hist); })
         .aggregate({sm::shard_label})});
}

std::ostream& operator<<(std::ostream& o, const server_configuration& c) {
    o << "{";
    for (auto& a : c.addrs) {
        o << a;
    }
    o << ", max_service_memory_per_core: " << c.max_service_memory_per_core
      << ", metrics_enabled:" << !c.disable_metrics;
    return o << "}";
}

std::ostream& operator<<(std::ostream& os, const server_endpoint& ep) {
    /**
     * We use simmillar syntax to kafka to indicate if endpoint is secured f.e.:
     *
     * SECURED://127.0.0.1:9092
     */
    fmt::print(
      os,
      "{{{}://{}:{}}}",
      ep.name,
      ep.addr,
      ep.credentials ? "SECURED" : "PLAINTEXT");
    return os;
}

} // namespace net
