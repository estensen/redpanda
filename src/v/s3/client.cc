/*
 * Copyright 2020 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#include "s3/client.h"

#include "bytes/iobuf.h"
#include "bytes/iobuf_istreambuf.h"
#include "bytes/iobuf_parser.h"
#include "hashing/secure.h"
#include "http/client.h"
#include "net/tls.h"
#include "net/types.h"
#include "s3/error.h"
#include "s3/logger.h"
#include "ssx/sformat.h"
#include "vlog.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/log.hh>

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <gnutls/crypto.h>

#include <exception>
#include <utility>

namespace s3 {

// Close all connections that were used more than 5 seconds ago.
// AWS S3 endpoint has timeout of 10 seconds. But since we're supporting
// not only AWS S3 it makes sense to set timeout value a bit lower.
static constexpr ss::lowres_clock::duration default_max_idle_time
  = std::chrono::seconds(5);

struct aws_header_names {
    static constexpr boost::beast::string_view prefix = "prefix";
    static constexpr boost::beast::string_view start_after = "start-after";
    static constexpr boost::beast::string_view max_keys = "max-keys";
    static constexpr boost::beast::string_view x_amz_tagging = "x-amz-tagging";
    static constexpr boost::beast::string_view x_amz_request_id
      = "x-amz-request-id";
};

struct aws_header_values {
    static constexpr boost::beast::string_view user_agent
      = "redpanda.vectorized.io";
    static constexpr boost::beast::string_view text_plain = "text/plain";
};

// configuration //

static ss::sstring make_endpoint_url(
  const cloud_roles::aws_region_name& region,
  const std::optional<endpoint_url>& url_override) {
    if (url_override) {
        return url_override.value();
    }
    return ssx::sformat("s3.{}.amazonaws.com", region());
}

ss::future<configuration> configuration::make_configuration(
  const std::optional<cloud_roles::public_key_str>& pkey,
  const std::optional<cloud_roles::private_key_str>& skey,
  const cloud_roles::aws_region_name& region,
  const default_overrides& overrides,
  net::metrics_disabled disable_metrics,
  net::public_metrics_disabled disable_public_metrics) {
    configuration client_cfg;
    const auto endpoint_uri = make_endpoint_url(region, overrides.endpoint);
    client_cfg.tls_sni_hostname = endpoint_uri;
    // Setup credentials for TLS
    client_cfg.access_key = pkey;
    client_cfg.secret_key = skey;
    client_cfg.region = region;
    client_cfg.uri = access_point_uri(endpoint_uri);
    ss::tls::credentials_builder cred_builder;
    if (overrides.disable_tls == false) {
        // NOTE: this is a pre-defined gnutls priority string that
        // picks the the ciphersuites with 128-bit ciphers which
        // leads to up to 10x improvement in upload speed, compared
        // to 256-bit ciphers
        cred_builder.set_priority_string("PERFORMANCE");
        if (overrides.trust_file.has_value()) {
            auto file = overrides.trust_file.value();
            vlog(s3_log.info, "Use non-default trust file {}", file());
            co_await cred_builder.set_x509_trust_file(
              file().string(), ss::tls::x509_crt_format::PEM);
        } else {
            // Use GnuTLS defaults, might not work on all systems
            auto ca_file = co_await net::find_ca_file();
            if (ca_file) {
                vlog(
                  s3_log.info,
                  "Use automatically discovered trust file {}",
                  ca_file.value());
                co_await cred_builder.set_x509_trust_file(
                  ca_file.value(), ss::tls::x509_crt_format::PEM);
            } else {
                vlog(
                  s3_log.info,
                  "Trust file can't be detected automatically, using GnuTLS "
                  "default");
                co_await cred_builder.set_system_trust();
            }
        }
        client_cfg.credentials
          = co_await cred_builder.build_reloadable_certificate_credentials();
    }

    constexpr uint16_t default_port = 443;

    client_cfg.server_addr = net::unresolved_address(
      client_cfg.uri(),
      overrides.port ? *overrides.port : default_port,
      ss::net::inet_address::family::INET);
    client_cfg.disable_metrics = disable_metrics;
    client_cfg.disable_public_metrics = disable_public_metrics;
    client_cfg._probe = ss::make_shared<client_probe>(
      disable_metrics, region(), endpoint_uri);
    client_cfg.max_idle_time = overrides.max_idle_time
                                 ? *overrides.max_idle_time
                                 : default_max_idle_time;
    co_return client_cfg;
}

std::ostream& operator<<(std::ostream& o, const configuration& c) {
    o << "{access_key:"
      << c.access_key.value_or(cloud_roles::public_key_str{""})
      << ",region:" << c.region() << ",secret_key:****"
      << ",access_point_uri:" << c.uri() << ",server_addr:" << c.server_addr
      << ",max_idle_time:"
      << std::chrono::duration_cast<std::chrono::milliseconds>(c.max_idle_time)
           .count()
      << "}";
    return o;
}

// request_creator //

request_creator::request_creator(
  const configuration& conf,
  ss::lw_shared_ptr<const cloud_roles::apply_credentials> apply_credentials)
  : _ap(conf.uri)
  , _apply_credentials{std::move(apply_credentials)} {}

result<http::client::request_header> request_creator::make_get_object_request(
  bucket_name const& name, object_key const& key) {
    http::client::request_header header{};
    // GET /{object-id} HTTP/1.1
    // Host: {bucket-name}.s3.amazonaws.com
    // x-amz-date:{req-datetime}
    // Authorization:{signature}
    // x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto host = fmt::format("{}.{}", name(), _ap());
    auto target = fmt::format("/{}", key().string());
    header.method(boost::beast::http::verb::get);
    header.target(target);
    header.insert(
      boost::beast::http::field::user_agent, aws_header_values::user_agent);
    header.insert(boost::beast::http::field::host, host);
    header.insert(boost::beast::http::field::content_length, "0");
    auto ec = _apply_credentials->add_auth(header);
    if (ec) {
        return ec;
    }
    return header;
}

result<http::client::request_header> request_creator::make_head_object_request(
  bucket_name const& name, object_key const& key) {
    http::client::request_header header{};
    // HEAD /{object-id} HTTP/1.1
    // Host: {bucket-name}.s3.amazonaws.com
    // x-amz-date:{req-datetime}
    // Authorization:{signature}
    // x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto host = fmt::format("{}.{}", name(), _ap());
    auto target = fmt::format("/{}", key().string());
    header.method(boost::beast::http::verb::head);
    header.target(target);
    header.insert(
      boost::beast::http::field::user_agent, aws_header_values::user_agent);
    header.insert(boost::beast::http::field::host, host);
    header.insert(boost::beast::http::field::content_length, "0");
    auto ec = _apply_credentials->add_auth(header);
    if (ec) {
        return ec;
    }
    return header;
}

result<http::client::request_header>
request_creator::make_unsigned_put_object_request(
  bucket_name const& name,
  object_key const& key,
  size_t payload_size_bytes,
  const std::vector<object_tag>& tags) {
    // PUT /my-image.jpg HTTP/1.1
    // Host: myBucket.s3.<Region>.amazonaws.com
    // Date: Wed, 12 Oct 2009 17:50:00 GMT
    // Authorization: authorization string
    // Content-Type: text/plain
    // Content-Length: 11434
    // x-amz-meta-author: Janet
    // Expect: 100-continue
    // [11434 bytes of object data]
    http::client::request_header header{};
    auto host = fmt::format("{}.{}", name(), _ap());
    auto target = fmt::format("/{}", key().string());
    header.method(boost::beast::http::verb::put);
    header.target(target);
    header.insert(
      boost::beast::http::field::user_agent, aws_header_values::user_agent);
    header.insert(boost::beast::http::field::host, host);
    header.insert(
      boost::beast::http::field::content_type, aws_header_values::text_plain);
    header.insert(
      boost::beast::http::field::content_length,
      std::to_string(payload_size_bytes));

    if (!tags.empty()) {
        std::stringstream tstr;
        for (const auto& [key, val] : tags) {
            tstr << fmt::format("&{}={}", key, val);
        }
        header.insert(aws_header_names::x_amz_tagging, tstr.str().substr(1));
    }

    auto ec = _apply_credentials->add_auth(header);
    if (ec) {
        return ec;
    }
    return header;
}

result<http::client::request_header>
request_creator::make_list_objects_v2_request(
  const bucket_name& name,
  std::optional<object_key> prefix,
  std::optional<object_key> start_after,
  std::optional<size_t> max_keys) {
    // GET /?list-type=2&prefix=photos/2006/&delimiter=/ HTTP/1.1
    // Host: example-bucket.s3.<Region>.amazonaws.com
    // x-amz-date: 20160501T000433Z
    // Authorization: authorization string
    http::client::request_header header{};
    auto host = fmt::format("{}.{}", name(), _ap());
    auto target = fmt::format("/?list-type=2");
    header.method(boost::beast::http::verb::get);
    header.target(target);
    header.insert(
      boost::beast::http::field::user_agent, aws_header_values::user_agent);
    header.insert(boost::beast::http::field::host, host);
    header.insert(boost::beast::http::field::content_length, "0");

    if (prefix) {
        header.insert(aws_header_names::prefix, (*prefix)().string());
    }
    if (start_after) {
        header.insert(aws_header_names::start_after, (*start_after)().string());
    }
    if (max_keys) {
        header.insert(aws_header_names::start_after, std::to_string(*max_keys));
    }

    auto ec = _apply_credentials->add_auth(header);
    vlog(s3_log.trace, "ListObjectsV2:\n {}", http::redacted_header(header));
    if (ec) {
        return ec;
    }
    return header;
}

result<http::client::request_header>
request_creator::make_delete_object_request(
  bucket_name const& name, object_key const& key) {
    http::client::request_header header{};
    // DELETE /{object-id} HTTP/1.1
    // Host: {bucket-name}.s3.amazonaws.com
    // x-amz-date:{req-datetime}
    // Authorization:{signature}
    // x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    //
    // NOTE: x-amz-mfa, x-amz-bypass-governance-retention are not used for now
    auto host = fmt::format("{}.{}", name(), _ap());
    auto target = fmt::format("/{}", key().string());
    header.method(boost::beast::http::verb::delete_);
    header.target(target);
    header.insert(
      boost::beast::http::field::user_agent, aws_header_values::user_agent);
    header.insert(boost::beast::http::field::host, host);
    header.insert(boost::beast::http::field::content_length, "0");
    auto ec = _apply_credentials->add_auth(header);
    if (ec) {
        return ec;
    }
    return header;
}

// client //

static void log_buffer_with_rate_limiting(const char* msg, iobuf& buf) {
    static constexpr int buffer_size = 0x100;
    static constexpr auto rate_limit = std::chrono::seconds(1);
    thread_local static ss::logger::rate_limit rate(rate_limit);
    auto log_with_rate_limit = [](ss::logger::format_info fmt, auto... args) {
        s3_log.log(ss::log_level::warn, rate, fmt, args...);
    };
    iobuf_istreambuf strbuf(buf);
    std::istream stream(&strbuf);
    std::array<char, buffer_size> str{};
    auto sz = stream.readsome(str.data(), buffer_size);
    auto sview = std::string_view(str.data(), sz);
    vlog(log_with_rate_limit, "{}: {}", msg, sview);
}

/// Convert iobuf that contains xml data to boost::property_tree
static boost::property_tree::ptree iobuf_to_ptree(iobuf&& buf) {
    namespace pt = boost::property_tree;
    try {
        iobuf_istreambuf strbuf(buf);
        std::istream stream(&strbuf);
        pt::ptree res;
        pt::read_xml(stream, res);
        return res;
    } catch (...) {
        log_buffer_with_rate_limiting("unexpected reply", buf);
        vlog(s3_log.error, "!!parsing error {}", std::current_exception());
        throw;
    }
}

/// Parse timestamp in format that S3 uses
static std::chrono::system_clock::time_point
parse_timestamp(std::string_view sv) {
    std::tm tm = {};
    std::stringstream ss({sv.data(), sv.size()});
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S.Z%Z");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

static client::list_bucket_result iobuf_to_list_bucket_result(iobuf&& buf) {
    try {
        for (auto& frag : buf) {
            vlog(
              s3_log.trace,
              "iobuf_to_list_bucket_result part {}",
              ss::sstring{frag.get(), frag.size()});
        }
        client::list_bucket_result result;
        auto root = iobuf_to_ptree(std::move(buf));
        for (const auto& [tag, value] : root.get_child("ListBucketResult")) {
            if (tag == "Contents") {
                client::list_bucket_item item;
                for (const auto& [item_tag, item_value] : value) {
                    if (item_tag == "Key") {
                        item.key = item_value.get_value<ss::sstring>();
                    } else if (item_tag == "Size") {
                        item.size_bytes = item_value.get_value<size_t>();
                    } else if (item_tag == "LastModified") {
                        item.last_modified = parse_timestamp(
                          item_value.get_value<ss::sstring>());
                    } else if (item_tag == "ETag") {
                        item.etag = item_value.get_value<ss::sstring>();
                    }
                }
                result.contents.push_back(std::move(item));
            } else if (tag == "IsTruncated") {
                // read value as bool
                result.is_truncated = value.get_value<bool>();
            } else if (tag == "Prefix") {
                result.prefix = value.get_value<ss::sstring>("");
            }
        }
        return result;
    } catch (...) {
        vlog(s3_log.error, "!!error parse result {}", std::current_exception());
        throw;
    }
}

template<class ResultT = void>
ss::future<ResultT> parse_rest_error_response(iobuf&& buf) {
    try {
        auto resp = iobuf_to_ptree(std::move(buf));
        constexpr const char* empty = "";
        auto code = resp.get<ss::sstring>("Error.Code", empty);
        auto msg = resp.get<ss::sstring>("Error.Message", empty);
        auto rid = resp.get<ss::sstring>("Error.RequestId", empty);
        auto res = resp.get<ss::sstring>("Error.Resource", empty);
        rest_error_response err(code, msg, rid, res);
        return ss::make_exception_future<ResultT>(err);
    } catch (...) {
        vlog(s3_log.error, "!!error parse error {}", std::current_exception());
        throw;
    }
}

/// Head response doesn't give us an XML encoded error object in
/// the body. This method uses headers to generate an error object.
template<class ResultT = void>
ss::future<ResultT> parse_head_error_response(
  const http::http_response::header_type& hdr, const object_key& key) {
    try {
        ss::sstring code;
        ss::sstring msg;
        if (hdr.result() == boost::beast::http::status::not_found) {
            code = "NoSuchKey";
            msg = "Not found";
        } else {
            code = "Unknown";
            msg = ss::sstring(hdr.reason().data(), hdr.reason().size());
        }
        auto rid = hdr.at(aws_header_names::x_amz_request_id);
        rest_error_response err(
          code, msg, ss::sstring(rid.data(), rid.size()), key().native());
        return ss::make_exception_future<ResultT>(err);
    } catch (...) {
        vlog(s3_log.error, "!!error parse error {}", std::current_exception());
        throw;
    }
}

static ss::future<iobuf>
drain_response_stream(http::client::response_stream_ref resp) {
    return ss::do_with(
      iobuf(), [resp = std::move(resp)](iobuf& outbuf) mutable {
          return ss::do_until(
                   [resp] { return resp->is_done(); },
                   [resp, &outbuf] {
                       return resp->recv_some().then([&outbuf](iobuf&& chunk) {
                           outbuf.append(std::move(chunk));
                       });
                   })
            .then([&outbuf] {
                return ss::make_ready_future<iobuf>(std::move(outbuf));
            });
      });
}

client::client(
  const configuration& conf,
  ss::lw_shared_ptr<const cloud_roles::apply_credentials> apply_credentials)
  : _requestor(conf, std::move(apply_credentials))
  , _client(conf)
  , _probe(conf._probe) {}

client::client(
  const configuration& conf,
  const ss::abort_source& as,
  ss::lw_shared_ptr<const cloud_roles::apply_credentials> apply_credentials)
  : _requestor(conf, std::move(apply_credentials))
  , _client(conf, &as, conf._probe, conf.max_idle_time)
  , _probe(conf._probe) {}

ss::future<> client::stop() { return _client.stop(); }

ss::future<> client::shutdown() {
    _client.shutdown();
    return ss::now();
}

ss::future<http::client::response_stream_ref> client::get_object(
  bucket_name const& name,
  object_key const& key,
  const ss::lowres_clock::duration& timeout) {
    auto header = _requestor.make_get_object_request(name, key);
    if (!header) {
        return ss::make_exception_future<http::client::response_stream_ref>(
          std::system_error(header.error()));
    }
    vlog(
      s3_log.trace,
      "send https request:\n{}",
      http::redacted_header(header.value()));
    return _client.request(std::move(header.value()), timeout)
      .then([](http::client::response_stream_ref&& ref) {
          // here we didn't receive any bytes from the socket and
          // ref->is_header_done() is 'false', we need to prefetch
          // the header first
          return ref->prefetch_headers().then([ref = std::move(ref)]() mutable {
              vassert(ref->is_header_done(), "Header is not received");
              if (
                ref->get_headers().result() != boost::beast::http::status::ok) {
                  // Got error response, consume the response body and produce
                  // rest api error
                  vlog(
                    s3_log.warn,
                    "S3 replied with error: {}",
                    ref->get_headers());
                  return drain_response_stream(std::move(ref))
                    .then([](iobuf&& res) {
                        return parse_rest_error_response<
                          http::client::response_stream_ref>(std::move(res));
                    });
              }
              return ss::make_ready_future<http::client::response_stream_ref>(
                std::move(ref));
          });
      });
}

ss::future<client::head_object_result> client::head_object(
  bucket_name const& name,
  object_key const& key,
  const ss::lowres_clock::duration& timeout) {
    auto header = _requestor.make_head_object_request(name, key);
    if (!header) {
        return ss::make_exception_future<client::head_object_result>(
          std::system_error(header.error()));
    }
    vlog(
      s3_log.trace,
      "send https request:\n{}",
      http::redacted_header(header.value()));
    return _client.request(std::move(header.value()), timeout)
      .then(
        [key](const http::client::response_stream_ref& ref)
          -> ss::future<head_object_result> {
            return ref->prefetch_headers().then(
              [ref, key]() -> ss::future<head_object_result> {
                  auto status = ref->get_headers().result();
                  if (status != boost::beast::http::status::ok) {
                      vlog(
                        s3_log.warn,
                        "S3 replied with error: {}",
                        ref->get_headers());
                      return parse_head_error_response<head_object_result>(
                        ref->get_headers(), key);
                  }
                  auto len = boost::lexical_cast<uint64_t>(
                    ref->get_headers().at(
                      boost::beast::http::field::content_length));
                  auto etag = ref->get_headers().at(
                    boost::beast::http::field::etag);
                  head_object_result results{
                    .object_size = len,
                    .etag = ss::sstring(etag.data(), etag.length()),
                  };
                  return ss::make_ready_future<head_object_result>(
                    std::move(results));
              });
        })
      .handle_exception_type([this](const rest_error_response& err) {
          _probe->register_failure(err.code());
          return ss::make_exception_future<head_object_result>(err);
      });
}

ss::future<> client::put_object(
  bucket_name const& name,
  object_key const& id,
  size_t payload_size,
  ss::input_stream<char>&& body,
  const std::vector<object_tag>& tags,
  const ss::lowres_clock::duration& timeout) {
    auto header = _requestor.make_unsigned_put_object_request(
      name, id, payload_size, tags);
    if (!header) {
        return ss::make_exception_future<>(std::system_error(header.error()));
    }
    vlog(
      s3_log.trace,
      "send https request:\n{}",
      http::redacted_header(header.value()));
    return ss::do_with(
      std::move(body),
      [this, timeout, header = std::move(header)](
        ss::input_stream<char>& body) mutable {
          return _client.request(std::move(header.value()), body, timeout)
            .then([](const http::client::response_stream_ref& ref) {
                return drain_response_stream(ref).then([ref](iobuf&& res) {
                    auto status = ref->get_headers().result();
                    if (status != boost::beast::http::status::ok) {
                        vlog(
                          s3_log.warn,
                          "S3 replied with error: {}",
                          ref->get_headers());
                        return parse_rest_error_response<>(std::move(res));
                    }
                    return ss::now();
                });
            })
            .handle_exception_type(
              [](const ss::abort_requested_exception&) { return ss::now(); })
            .handle_exception_type([this](const rest_error_response& err) {
                _probe->register_failure(err.code());
                return ss::make_exception_future<>(err);
            })
            .finally([&body]() { return body.close(); });
      });
}

ss::future<client::list_bucket_result> client::list_objects_v2(
  const bucket_name& name,
  std::optional<object_key> prefix,
  std::optional<object_key> start_after,
  std::optional<size_t> max_keys,
  const ss::lowres_clock::duration& timeout) {
    auto header = _requestor.make_list_objects_v2_request(
      name, std::move(prefix), std::move(start_after), max_keys);
    if (!header) {
        return ss::make_exception_future<list_bucket_result>(
          std::system_error(header.error()));
    }
    vlog(
      s3_log.trace,
      "send https request:\n{}",
      http::redacted_header(header.value()));
    return _client.request(std::move(header.value()), timeout)
      .then([](const http::client::response_stream_ref& resp) mutable {
          // chunked encoding is used so we don't know output size in
          // advance
          return ss::do_with(
            resp->as_input_stream(),
            iobuf(),
            [resp](ss::input_stream<char>& stream, iobuf& outbuf) mutable {
                return ss::do_until(
                         [&stream] { return stream.eof(); },
                         [&stream, &outbuf] {
                             return stream.read().then(
                               [&outbuf](ss::temporary_buffer<char>&& chunk) {
                                   outbuf.append(std::move(chunk));
                               });
                         })
                  .then([&outbuf, resp] {
                      const auto& header = resp->get_headers();
                      if (header.result() != boost::beast::http::status::ok) {
                          // We received error response so the outbuf contains
                          // error digest instead of the result of the query
                          vlog(
                            s3_log.warn, "S3 replied with error: {}", header);
                          return parse_rest_error_response<
                            client::list_bucket_result>(std::move(outbuf));
                      }
                      auto res = iobuf_to_list_bucket_result(std::move(outbuf));
                      return ss::make_ready_future<list_bucket_result>(
                        std::move(res));
                  });
            });
      });
}

ss::future<> client::delete_object(
  const bucket_name& bucket,
  const object_key& key,
  const ss::lowres_clock::duration& timeout) {
    auto header = _requestor.make_delete_object_request(bucket, key);
    if (!header) {
        return ss::make_exception_future<>(std::system_error(header.error()));
    }
    vlog(
      s3_log.trace,
      "send https request:\n{}",
      http::redacted_header(header.value()));
    return _client.request(std::move(header.value()), timeout)
      .then([](const http::client::response_stream_ref& ref) {
          return drain_response_stream(ref).then([ref](iobuf&& res) {
              auto status = ref->get_headers().result();
              if (
                status != boost::beast::http::status::ok
                && status
                     != boost::beast::http::status::no_content) { // expect 204
                  vlog(
                    s3_log.warn,
                    "S3 replied with error: {}",
                    ref->get_headers());
                  return parse_rest_error_response<>(std::move(res));
              }
              return ss::now();
          });
      });
}

client_pool::client_pool(
  size_t size, configuration conf, client_pool_overdraft_policy policy)
  : _max_size(size)
  , _config(std::move(conf))
  , _policy(policy) {}

ss::future<> client_pool::stop() {
    _as.request_abort();
    _cvar.broken();
    // Wait until all leased objects are returned
    co_await _gate.close();
}

/// \brief Acquire http client from the pool.
///
/// \note it's guaranteed that the client can only be acquired once
///       before it gets released (release happens implicitly, when
///       the lifetime of the pointer ends).
/// \return client pointer (via future that can wait if all clients
///         are in use)
ss::future<client_pool::client_lease> client_pool::acquire() {
    gate_guard guard(_gate);
    try {
        // If credentials have not yet been acquired, wait for them. It is
        // possible that credentials are not initialized right after remote
        // starts, and we have not had a response from the credentials API yet,
        // but we have scheduled an upload. This wait ensures that when we call
        // the storage API we have a set of valid credentials.
        if (unlikely(!_apply_credentials)) {
            co_await wait_for_credentials();
        }

        while (_pool.empty() && !_gate.is_closed()) {
            if (_policy == client_pool_overdraft_policy::wait_if_empty) {
                co_await _cvar.wait();
            } else {
                auto cl = ss::make_shared<client>(
                  _config, _as, _apply_credentials);
                _pool.emplace_back(std::move(cl));
            }
        }
    } catch (const ss::broken_condition_variable&) {
    }
    if (_gate.is_closed() || _as.abort_requested()) {
        throw ss::gate_closed_exception();
    }
    vassert(!_pool.empty(), "'acquire' invariant is broken");
    auto client = _pool.back();
    _pool.pop_back();
    co_return client_lease{
      .client = client,
      .deleter = ss::make_deleter(
        [pool = weak_from_this(), client, g = std::move(guard)] {
            if (pool) {
                pool->release(client);
            }
        })};
}

size_t client_pool::size() const noexcept { return _pool.size(); }

size_t client_pool::max_size() const noexcept { return _max_size; }

void client_pool::populate_client_pool() {
    for (size_t i = 0; i < _max_size; i++) {
        auto cl = ss::make_shared<client>(_config, _as, _apply_credentials);
        _pool.emplace_back(std::move(cl));
    }
}

void client_pool::release(ss::shared_ptr<client> leased) {
    if (_pool.size() == _max_size) {
        return;
    }
    _pool.emplace_back(std::move(leased));
    _cvar.signal();
}

void client_pool::load_credentials(cloud_roles::credentials credentials) {
    if (unlikely(!_apply_credentials)) {
        _apply_credentials = ss::make_lw_shared(
          cloud_roles::make_credentials_applier(std::move(credentials)));
        populate_client_pool();
        // We signal the waiter only after the client pool is initialized, so
        // that any upload operations waiting are ready to proceed.
        _credentials_var.signal();
    } else {
        _apply_credentials->reset_creds(std::move(credentials));
    }
}

ss::future<> client_pool::wait_for_credentials() {
    co_await _credentials_var.wait([this]() {
        return _gate.is_closed() || _as.abort_requested()
               || (bool{_apply_credentials} && !_pool.empty());
    });

    if (_gate.is_closed() || _as.abort_requested()) {
        throw ss::gate_closed_exception();
    }
    co_return;
}

} // namespace s3
