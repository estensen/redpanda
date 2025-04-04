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
#include "kafka/server/connection_context.h"

#include "bytes/iobuf.h"
#include "config/configuration.h"
#include "kafka/protocol/sasl_authenticate.h"
#include "kafka/server/handlers/handler_interface.h"
#include "kafka/server/protocol.h"
#include "kafka/server/protocol_utils.h"
#include "kafka/server/quota_manager.h"
#include "kafka/server/request_context.h"
#include "kafka/server/response.h"
#include "security/exceptions.h"
#include "units.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace kafka {

ss::future<> connection_context::process_one_request() {
    return parse_size(_rs.conn->input())
      .then([this](std::optional<size_t> sz) mutable {
          if (!sz) {
              return ss::make_ready_future<>();
          }
          /*
           * Intercept the wire protocol when:
           *
           * 1. sasl is enabled (implied by 2)
           * 2. during auth phase
           * 3. handshake was v0
           */
          if (unlikely(
                sasl().state()
                  == security::sasl_server::sasl_state::authenticate
                && sasl().handshake_v0())) {
              return handle_auth_v0(*sz).handle_exception(
                [this](std::exception_ptr e) {
                    vlog(klog.info, "Detected error processing request: {}", e);
                    _rs.conn->shutdown_input();
                });
          }
          return parse_header(_rs.conn->input())
            .then(
              [this, s = sz.value()](std::optional<request_header> h) mutable {
                  _rs.probe().add_bytes_received(s);
                  if (!h) {
                      vlog(
                        klog.debug,
                        "could not parse header from client: {}",
                        _rs.conn->addr);
                      _rs.probe().header_corrupted();
                      return ss::make_ready_future<>();
                  }
                  return dispatch_method_once(std::move(h.value()), s)
                    .handle_exception_type([this](const std::bad_alloc&) {
                        // In general, dispatch_method_once does not throw,
                        // but bad_allocs are an exception.  Log it cleanly
                        // to avoid this bubbling up as an unhandled
                        // exceptional future.
                        vlog(
                          klog.error,
                          "Request from {} failed on memory exhaustion "
                          "(std::bad_alloc)",
                          _rs.conn->addr);
                    });
              });
      });
}

/*
 * The SASL authentication flow for a client using version 0 of SASL handshake
 * doesn't use an envelope request for tokens. This method intercepts the
 * authentication phase and builds an envelope request so that all of the normal
 * request processing can be re-used.
 *
 * Even though we build and decode a request/response, the payload is a small
 * authentication string. https://github.com/redpanda-data/redpanda/issues/1315.
 * When this ticket is complete we'll be able to easily remove this extra
 * serialization step and and easily operate on non-encoded requests/responses.
 */
ss::future<> connection_context::handle_auth_v0(const size_t size) {
    vlog(klog.debug, "Processing simulated SASL authentication request");

    /*
     * very generous upper bound for some added safety. generally the size is
     * small and corresponds to the representation of hashes being exchanged but
     * there is some flexibility as usernames, nonces, etc... have no strict
     * limits. future non-SCRAM mechanisms may have other size requirements.
     */
    if (unlikely(size > 256_KiB)) {
        throw std::runtime_error(fmt_with_ctx(
          fmt::format, "Auth (handshake_v0) message too large: {}", size));
    }

    const api_version version(0);
    iobuf request_buf;
    {
        auto data = co_await read_iobuf_exactly(_rs.conn->input(), size);
        sasl_authenticate_request request;
        request.data.auth_bytes = iobuf_to_bytes(data);
        response_writer writer(request_buf);
        request.encode(writer, version);
    }

    sasl_authenticate_response response;
    {
        auto ctx = request_context(
          shared_from_this(),
          request_header{
            .key = sasl_authenticate_api::key,
            .version = version,
          },
          std::move(request_buf),
          0s);
        auto sres = session_resources{};
        auto resp = co_await kafka::process_request(
                      std::move(ctx), _proto.smp_group(), sres)
                      .response;
        auto data = std::move(*resp).release();
        response.decode(std::move(data), version);
    }

    if (response.data.error_code != error_code::none) {
        throw std::runtime_error(fmt_with_ctx(
          fmt::format,
          "Auth (handshake v0) error {}: {}",
          response.data.error_code,
          response.data.error_message));
    }

    if (sasl().state() == security::sasl_server::sasl_state::failed) {
        throw std::runtime_error(fmt_with_ctx(
          fmt::format, "Auth (handshake v0) failed with unknown error"));
    }

    iobuf data;
    response_writer writer(data);
    writer.write(response.data.auth_bytes);
    auto msg = iobuf_as_scattered(std::move(data));
    co_await _rs.conn->write(std::move(msg));
}

bool connection_context::is_finished_parsing() const {
    return _rs.conn->input().eof() || _rs.abort_requested();
}

ss::future<session_resources> connection_context::throttle_request(
  const request_header& hdr, size_t request_size) {
    // update the throughput tracker for this client using the
    // size of the current request and return any computed delay
    // to apply for quota throttling.
    //
    // note that when throttling is first applied the request is
    // allowed to pass through and subsequent requests and
    // delayed. this is a similar strategy used by kafka: the
    // response is important because it allows clients to
    // distinguish throttling delays from real delays. delays
    // applied to subsequent messages allow backpressure to take
    // affect.
    auto delay = _proto.quota_mgr().record_tp_and_throttle(
      hdr.client_id, request_size);
    auto tracker = std::make_unique<request_tracker>(_rs.probe());
    auto fut = ss::now();
    if (!delay.first_violation) {
        fut = ss::sleep_abortable(delay.duration, _rs.abort_source());
    }
    auto track = track_latency(hdr.key);
    return fut
      .then([this, key = hdr.key, request_size] {
          return reserve_request_units(key, request_size);
      })
      .then([this, delay, track, tracker = std::move(tracker)](
              ss::semaphore_units<> units) mutable {
          return server().get_request_unit().then(
            [this,
             delay,
             mem_units = std::move(units),
             track,
             tracker = std::move(tracker)](
              ss::semaphore_units<> qd_units) mutable {
                session_resources r{
                  .backpressure_delay = delay.duration,
                  .memlocks = std::move(mem_units),
                  .queue_units = std::move(qd_units),
                  .tracker = std::move(tracker),
                };
                if (track) {
                    r.method_latency = _rs.hist().auto_measure();
                }
                return r;
            });
      });
}

ss::future<ss::semaphore_units<>>
connection_context::reserve_request_units(api_key key, size_t size) {
    // Defer to the handler for the request type for the memory estimate, but
    // if the request isn't found, use the default estimate (although in that
    // case the request is likely for an API we don't support or malformed, so
    // it is likely to fail shortly anyway).
    auto handler = handler_for_key(key);
    auto mem_estimate = handler ? (*handler)->memory_estimate(size, *this)
                                : default_memory_estimate(size);
    if (unlikely(mem_estimate >= (size_t)std::numeric_limits<int32_t>::max())) {
        // TODO: Create error response using the specific API?
        throw std::runtime_error(fmt::format(
          "request too large > 1GB (size: {}, estimate: {}, API: {})",
          size,
          mem_estimate,
          handler ? (*handler)->name() : "<bad key>"));
    }
    auto fut = ss::get_units(_rs.memory(), mem_estimate);
    if (_rs.memory().waiters()) {
        _rs.probe().waiting_for_available_memory();
    }
    return fut;
}

ss::future<>
connection_context::dispatch_method_once(request_header hdr, size_t size) {
    return throttle_request(hdr, size).then([this, hdr = std::move(hdr), size](
                                              session_resources
                                                sres_in) mutable {
        if (_rs.abort_requested()) {
            // protect against shutdown behavior
            return ss::make_ready_future<>();
        }

        auto sres = ss::make_lw_shared(std::move(sres_in));

        auto remaining = size - request_header_size
                         - hdr.client_id_buffer.size() - hdr.tags_size_bytes;
        return read_iobuf_exactly(_rs.conn->input(), remaining)
          .then([this, hdr = std::move(hdr), sres = std::move(sres)](
                  iobuf buf) mutable {
              if (_rs.abort_requested()) {
                  // _proto._cntrl etc might not be alive
                  return ss::now();
              }
              auto self = shared_from_this();
              auto rctx = request_context(
                self, std::move(hdr), std::move(buf), sres->backpressure_delay);
              /*
               * we process requests in order since all subsequent requests
               * are dependent on authentication having completed.
               *
               * the other important reason for disabling pipeling is because
               * when a sasl handshake with version=0 is processed, the next
               * data on the wire is _not_ another request: it is a
               * size-prefixed authentication payload without a request
               * envelope, and requires special handling.
               *
               * a well behaved client should implicitly provide a data stream
               * that invokes this behavior in the server: that is, it won't
               * send auth data (or any other requests) until handshake or the
               * full auth-process completes, etc... but representing these
               * nuances of the protocol _explicitly_ in the server makes its
               * behavior easier to understand and avoids misbehaving clients
               * creating server-side errors that will appear as a corrupted
               * stream at best and at worst some odd behavior.
               */

              const auto correlation = rctx.header().correlation;
              const sequence_id seq = _seq_idx;
              _seq_idx = _seq_idx + sequence_id(1);
              auto res = kafka::process_request(
                std::move(rctx), _proto.smp_group(), *sres);
              /**
               * first stage processed in a foreground.
               */
              return res.dispatched
                .then_wrapped([this,
                               f = std::move(res.response),
                               seq,
                               correlation,
                               self,
                               sres = std::move(sres)](ss::future<> d) mutable {
                    /*
                     * if the dispatch/first stage failed, then we need to
                     * need to consume the second stage since it might be
                     * an exceptional future. if we captured `f` in the
                     * lambda but didn't use `then_wrapped` then the
                     * lambda would be destroyed and an ignored
                     * exceptional future would be caught by seastar.
                     */
                    if (d.failed()) {
                        return f.discard_result()
                          .handle_exception([](std::exception_ptr e) {
                              vlog(
                                klog.info,
                                "Discarding second stage failure {}",
                                e);
                          })
                          .finally([self, d = std::move(d)]() mutable {
                              self->_rs.probe().service_error();
                              self->_rs.probe().request_completed();
                              return std::move(d);
                          });
                    }
                    /**
                     * second stage processed in background.
                     */
                    ssx::background
                      = ssx::spawn_with_gate_then(
                          _rs.conn_gate(),
                          [this,
                           f = std::move(f),
                           sres = std::move(sres),
                           seq,
                           correlation]() mutable {
                              return f.then(
                                [this,
                                 sres = std::move(sres),
                                 seq,
                                 correlation](response_ptr r) mutable {
                                    r->set_correlation(correlation);
                                    response_and_resources randr{
                                      std::move(r), std::move(sres)};
                                    _responses.insert({seq, std::move(randr)});
                                    return maybe_process_responses();
                                });
                          })
                          .handle_exception([self](std::exception_ptr e) {
                              // ssx::spawn_with_gate already caught
                              // shutdown-like exceptions, so we should only be
                              // taking this path for real errors.  That also
                              // means that on shutdown we don't bother to call
                              // shutdown_input on the connection, so rely
                              // on any future reader to check the abort
                              // source before considering reading the
                              // connection.

                              auto disconnected = net::is_disconnect_exception(
                                e);
                              if (disconnected) {
                                  vlog(
                                    klog.info,
                                    "Disconnected {} ({})",
                                    self->_rs.conn->addr,
                                    disconnected.value());
                              } else {
                                  vlog(
                                    klog.warn,
                                    "Error processing request: {}",
                                    e);
                              }

                              self->_rs.probe().service_error();
                              self->_rs.conn->shutdown_input();
                          });
                    return d;
                })
                .handle_exception([self](std::exception_ptr e) {
                    vlog(
                      klog.info, "Detected error dispatching request: {}", e);
                    self->_rs.conn->shutdown_input();
                });
          });
    });
}

/**
 * This method processes as many responses as possible, in request order. Since
 * we proces the second stage asynchronously within a given connection, reponses
 * may become ready out of order, but Kafka clients expect responses exactly in
 * request order.
 *
 * The _responses queue handles that: responses are enqueued there in completion
 * order, but only sent to the client in response order. So this method, called
 * after every response is ready, may end up sending zero, one or more requests,
 * depending on the completion order.
 *
 * @return ss::future<>
 */
ss::future<> connection_context::maybe_process_responses() {
    return ss::repeat([this]() mutable {
        auto it = _responses.find(_next_response);
        if (it == _responses.end()) {
            return ss::make_ready_future<ss::stop_iteration>(
              ss::stop_iteration::yes);
        }
        // found one; increment counter
        _next_response = _next_response + sequence_id(1);

        auto resp_and_res = std::move(it->second);

        _responses.erase(it);

        if (resp_and_res.response->is_noop()) {
            return ss::make_ready_future<ss::stop_iteration>(
              ss::stop_iteration::no);
        }

        auto msg = response_as_scattered(std::move(resp_and_res.response));
        try {
            return _rs.conn->write(std::move(msg))
              .then([] {
                  return ss::make_ready_future<ss::stop_iteration>(
                    ss::stop_iteration::no);
              })
              // release the resources only once it has been written to the
              // connection.
              .finally([resources = std::move(resp_and_res.resources)] {});
        } catch (...) {
            vlog(
              klog.debug,
              "Failed to process request: {}",
              std::current_exception());
        }
        return ss::make_ready_future<ss::stop_iteration>(
          ss::stop_iteration::no);
    });
}

} // namespace kafka
