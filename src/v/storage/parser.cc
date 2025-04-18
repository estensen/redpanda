// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "storage/parser.h"

#include "bytes/iobuf.h"
#include "bytes/iobuf_parser.h"
#include "bytes/utils.h"
#include "likely.h"
#include "model/record.h"
#include "model/record_utils.h"
#include "reflection/adl.h"
#include "storage/logger.h"
#include "storage/parser.h"
#include "storage/parser_utils.h"
#include "storage/segment_appender_utils.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/variant_utils.hh>

#include <bits/stdint-uintn.h>
#include <fmt/format.h>

#include <algorithm>
#include <exception>

namespace storage {
using stop_parser = batch_consumer::stop_parser;

static model::record_batch_header header_from_iobuf(iobuf b) {
    iobuf_parser parser(std::move(b));
    auto header_crc = reflection::adl<uint32_t>{}.from(parser);
    auto sz = reflection::adl<int32_t>{}.from(parser);
    using offset_t = model::offset::type;
    auto off = model::offset(reflection::adl<offset_t>{}.from(parser));
    auto type = reflection::adl<model::record_batch_type>{}.from(parser);
    auto crc = reflection::adl<int32_t>{}.from(parser);
    using attr_t = model::record_batch_attributes::type;
    auto attrs = model::record_batch_attributes(
      reflection::adl<attr_t>{}.from(parser));
    auto delta = reflection::adl<int32_t>{}.from(parser);
    using tmstmp_t = model::timestamp::type;
    auto first = model::timestamp(reflection::adl<tmstmp_t>{}.from(parser));
    auto max = model::timestamp(reflection::adl<tmstmp_t>{}.from(parser));
    auto producer_id = reflection::adl<int64_t>{}.from(parser);
    auto producer_epoch = reflection::adl<int16_t>{}.from(parser);
    auto base_sequence = reflection::adl<int32_t>{}.from(parser);
    auto record_count = reflection::adl<int32_t>{}.from(parser);
    vassert(
      parser.bytes_consumed() == model::packed_record_batch_header_size,
      "Error in header parsing. Must consume:{} bytes, but consumed:{}",
      model::packed_record_batch_header_size,
      parser.bytes_consumed());
    auto hdr = model::record_batch_header{
      .header_crc = header_crc,
      .size_bytes = sz,
      .base_offset = off,
      .type = type,
      .crc = crc,
      .attrs = attrs,
      .last_offset_delta = delta,
      .first_timestamp = first,
      .max_timestamp = max,
      .producer_id = producer_id,
      .producer_epoch = producer_epoch,
      .base_sequence = base_sequence,
      .record_count = record_count};
    hdr.ctx.owner_shard = ss::this_shard_id();
    return hdr;
}

static ss::future<result<iobuf>> verify_read_iobuf(
  ss::input_stream<char>& in,
  size_t expected,
  ss::sstring msg,
  bool recover = false) {
    return read_iobuf_exactly(in, expected)
      .then([msg = std::move(msg), expected, recover](iobuf b) {
          if (likely(b.size_bytes() == expected)) {
              return ss::make_ready_future<result<iobuf>>(std::move(b));
          }
          if (!recover) {
              stlog.error(
                "cannot continue parsing. recived size:{} bytes, expected:{} "
                "bytes. context:{}",
                b.size_bytes(),
                expected,
                msg);
          } else {
              stlog.debug(
                "recovery ended with short read. recived size:{} bytes, "
                "expected:{} "
                "bytes. context:{}",
                b.size_bytes(),
                expected,
                msg);
          }
          return ss::make_ready_future<result<iobuf>>(
            parser_errc::input_stream_not_enough_bytes);
      });
}

ss::future<result<stop_parser>> continuous_batch_parser::consume_header() {
    /**
     * we use a loop to prevent using tail recursion
     **/
    for (;;) {
        if (!_header) {
            auto r = co_await read_header();
            if (!r) {
                co_return r.error();
            }
            _header = r.value();
        }

        auto ret = _consumer->accept_batch_start(*_header);
        switch (ret) {
        case batch_consumer::consume_result::stop_parser:
            co_return stop_parser::yes;
        case batch_consumer::consume_result::accept_batch:
            _consumer->consume_batch_start(
              *_header, _physical_base_offset, _header->size_bytes);
            _physical_base_offset += _header->size_bytes;
            co_return stop_parser::no;
        case batch_consumer::consume_result::skip_batch:
            _consumer->skip_batch_start(
              *_header, _physical_base_offset, _header->size_bytes);
            _physical_base_offset += _header->size_bytes;
            auto remaining = _header->size_bytes
                             - model::packed_record_batch_header_size;
            auto b = co_await verify_read_iobuf(
              get_stream(), remaining, "parser::skip_batch", _recovery);

            if (!b) {
                co_return b.error();
            }
            // start again
            add_bytes_and_reset();
            continue;
        }
        __builtin_unreachable();
    }
}

template<class Consumer>
static ss::future<result<model::record_batch_header>> read_header_impl(
  ss::input_stream<char>& input,
  const Consumer& consumer,
  bool recovery = false) {
    auto b = co_await read_iobuf_exactly(
      input, model::packed_record_batch_header_size);

    if (b.empty()) {
        // benign outcome. happens at end of file
        co_return parser_errc::end_of_stream;
    }
    if (b.size_bytes() != model::packed_record_batch_header_size) {
        if (!recovery) {
            stlog.error(
              "Could not parse header. Expected:{}, but Got:{}. consumer:{}",
              model::packed_record_batch_header_size,
              b.size_bytes(),
              consumer);
        } else {
            stlog.debug(
              "End of recovery with parse error. Expected:{}, but Got:{}. "
              "consumer:{})",
              model::packed_record_batch_header_size,
              b.size_bytes(),
              consumer);
        }
        co_return parser_errc::input_stream_not_enough_bytes;
    }
    // check if iobuf is filled is zeros, this means that we are reading
    // fallocated range filled with zeros
    if (unlikely(is_zero(b))) {
        // happens when we fallocate the file
        co_return parser_errc::fallocated_file_read_zero_bytes_for_header;
    }
    auto header = header_from_iobuf(std::move(b));

    if (auto computed_crc = model::internal_header_only_crc(header);
        unlikely(header.header_crc != computed_crc)) {
        if (!recovery) {
            vlog(
              stlog.error,
              "detected header corruption. stopping parser. Expected CRC of "
              "{}, but got header CRC: {} - {}. consumer:{}",
              computed_crc,
              header.header_crc,
              header,
              consumer);
        } else {
            vlog(
              stlog.debug,
              "End of recovery with CRC mismatch. Expected CRC of "
              "{}, but got header CRC: {} - {}. consumer:{}",
              computed_crc,
              header.header_crc,
              header,
              consumer);
        }
        co_return parser_errc::header_only_crc_missmatch;
    }
    co_return header;
}

ss::future<result<model::record_batch_header>>
continuous_batch_parser::read_header() {
    return read_header_impl(get_stream(), *_consumer, _recovery);
}

ss::future<result<stop_parser>> continuous_batch_parser::consume_one() {
    return consume_header().then([this](result<stop_parser> st) {
        if (!st) {
            return ss::make_ready_future<result<stop_parser>>(st.error());
        }
        if (st.value() == stop_parser::yes) {
            return ss::make_ready_future<result<stop_parser>>(st.value());
        }
        return consume_records().then([this](result<stop_parser> r) {
            add_bytes_and_reset();
            return r;
        });
    });
}

size_t continuous_batch_parser::consumed_batch_bytes() const {
    return _header->size_bytes;
}

void continuous_batch_parser::add_bytes_and_reset() {
    _bytes_consumed += consumed_batch_bytes();
    _header = {}; // reset
}
ss::future<result<stop_parser>> continuous_batch_parser::consume_records() {
    auto sz = _header->size_bytes - model::packed_record_batch_header_size;
    return verify_read_iobuf(
             get_stream(), sz, "parser::consume_records", _recovery)
      .then([this](result<iobuf> record) -> result<stop_parser> {
          if (!record) {
              return record.error();
          }
          _consumer->consume_records(std::move(record.value()));
          return result<stop_parser>(_consumer->consume_batch_end());
      });
}

ss::future<result<size_t>> continuous_batch_parser::consume() {
    if (unlikely(_err != parser_errc::none)) {
        return ss::make_ready_future<result<size_t>>(_err);
    }
    return ss::repeat([this] {
               return consume_one().then([this](result<stop_parser> s) {
                   if (!s) {
                       _err = parser_errc(s.error().value());
                       return ss::stop_iteration::yes;
                   }
                   if (get_stream().eof()) {
                       return ss::stop_iteration::yes;
                   }
                   if (s.value() == stop_parser::yes) {
                       return ss::stop_iteration::yes;
                   }
                   return ss::stop_iteration::no;
               });
           })
      .then([this] {
          if (_bytes_consumed) {
              // support partial reads
              return result<size_t>(_bytes_consumed);
          }
          constexpr std::array<parser_errc, 3> benign_error_codes{
            {parser_errc::none,
             parser_errc::end_of_stream,
             parser_errc::fallocated_file_read_zero_bytes_for_header}};
          if (std::any_of(
                benign_error_codes.begin(),
                benign_error_codes.end(),
                [v = _err](parser_errc e) { return e == v; })) {
              return result<size_t>(_bytes_consumed);
          }
          return result<size_t>(_err);
      });
}

class copy_helper {
public:
    explicit copy_helper(
      ss::input_stream<char> input,
      ss::output_stream<char> output,
      record_batch_transform_predicate pred)
      : _input(std::move(input))
      , _output(std::move(output))
      , _pred(std::move(pred)) {}

    ss::future<result<model::record_batch_header>> read_header() {
        return read_header_impl(_input, ss::sstring("copy_helper"));
    }

    /// Copy data.
    /// Return number of bytes copied.
    ss::future<result<size_t>> run() {
        size_t consumed = 0;
        bool stop = false;
        while (!stop) {
            auto r = co_await read_header();
            if (!r) {
                if (r.error() == parser_errc::end_of_stream) {
                    break;
                }
                co_return r.error();
            }

            _header = r.value();
            auto remaining = _header.size_bytes
                             - model::packed_record_batch_header_size;

            // invoke pred
            switch (_pred(_header)) {
            case batch_consumer::consume_result::skip_batch:
                // Record batch shouldn't be copied, we can just skip its
                // content altogether
                co_await _input.skip(remaining);
                break;
            case batch_consumer::consume_result::accept_batch: {
                auto body = co_await verify_read_iobuf(
                  _input, remaining, "copy_helper");
                if (!body) {
                    co_return body.error();
                }
                // we should only write to the output stream if we can
                // guarantee that both header and records are available

                // the header should be re-serialized since the predicate
                // might change it in-place (this is a low level tool)
                // we're also need to update header only crc
                _header.header_crc = model::internal_header_only_crc(_header);
                iobuf hdr = disk_header_to_iobuf(_header);
                co_await write_iobuf_to_output_stream(std::move(hdr), _output);
                co_await write_iobuf_to_output_stream(
                  std::move(body.value()), _output);
                consumed += _header.size_bytes;
                break;
            }
            case batch_consumer::consume_result::stop_parser:
                stop = true;
                break;
            };
        }
        co_await _output.flush();
        co_return consumed;
    }

    ss::future<> close() {
        auto input_close = _input.close();
        auto output_close = _output.close();
        auto [fi, fo] = co_await ss::when_all(
          std::move(input_close), std::move(output_close));
        if (fi.failed()) {
            vlog(
              stlog.error, "Input stram close error: {}", fi.get_exception());
        }
        if (fo.failed()) {
            vlog(
              stlog.error, "Output stram close error: {}", fo.get_exception());
        }
        if (fo.failed()) {
            std::rethrow_exception(fo.get_exception());
        }
        if (fi.failed()) {
            std::rethrow_exception(fi.get_exception());
        }
    }

    ss::input_stream<char> _input;
    ss::output_stream<char> _output;
    record_batch_transform_predicate _pred;
    model::record_batch_header _header{};
};

ss::future<result<size_t>> transform_stream(
  ss::input_stream<char> in,
  ss::output_stream<char> out,
  record_batch_transform_predicate pred) {
    copy_helper helper(std::move(in), std::move(out), std::move(pred));
    co_return co_await helper.run().finally(
      [&helper] { return helper.close(); });
}

} // namespace storage
