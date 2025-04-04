
// Copyright 2021 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#pragma once

#include "bytes/iobuf_parser.h"
#include "hashing/crc32c.h"
#include "likely.h"
#include "reflection/type_traits.h"
#include "serde/envelope_for_each_field.h"
#include "serde/logger.h"
#include "serde/serde_exception.h"
#include "serde/type_str.h"
#include "ssx/future-util.h"
#include "ssx/sformat.h"
#include "tristate.h"
#include "utils/fragmented_vector.h"
#include "utils/named_type.h"
#include "vlog.h"

#include <seastar/core/future.hh>
#include <seastar/net/inet_address.hh>

#include <absl/container/btree_set.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>

#include <iosfwd>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>

namespace serde {

template<typename T>
inline constexpr bool serde_is_enum_v =
#if __has_cpp_attribute(__cpp_lib_is_scoped_enum)
  std::is_scoped_enum_v<T>;
#else
  std::is_enum_v<T>;
#endif

#if defined(SERDE_TEST)
using serde_size_t = uint16_t;
#else
using serde_size_t = uint32_t;
#endif

using checksum_t = uint32_t;

template<typename To, typename From>
To bit_cast(From const& f) {
    static_assert(sizeof(From) == sizeof(To));
    static_assert(std::is_trivially_copyable_v<To>);
    To to;
    std::memcpy(&to, &f, sizeof(To));
    return to;
}

struct header {
    version_t _version, _compat_version;
    size_t _bytes_left_limit;
    checksum_t _checksum;
};

template<typename T>
concept has_serde_read = requires(T t, iobuf_parser& in, const header& h) {
    t.serde_read(in, h);
};

template<typename T>
concept has_serde_write = requires(T t, iobuf& out) {
    t.serde_write(out);
};

template<typename T>
concept has_serde_async_read
  = requires(T t, iobuf_parser& in, const header& h) {
    { t.serde_async_read(in, h) } -> seastar::Future;
};

template<typename T>
concept has_serde_async_write = requires(T t, iobuf& out) {
    { t.serde_async_write(out) } -> seastar::Future;
};

template<typename T>
concept is_absl_flat_hash_map
  = ::detail::is_specialization_of_v<T, absl::flat_hash_map>;

template<typename T>
concept is_absl_btree_set
  = ::detail::is_specialization_of_v<T, absl::btree_set>;

using serde_enum_serialized_t = int32_t;

#if defined(SERDE_TEST)
using serde_size_t = uint16_t;
#else
using serde_size_t = uint32_t;
#endif

namespace detail {

template<class T, template<class, size_t> class C>
struct is_specialization_of_sized : std::false_type {};
template<template<class, size_t> class C, class T, size_t N>
struct is_specialization_of_sized<C<T, N>, C> : std::true_type {};
template<typename T, template<class, size_t> class C>
inline constexpr bool is_specialization_of_sized_v
  = is_specialization_of_sized<T, C>::value;

} // namespace detail

template<typename T>
concept is_fragmented_vector
  = detail::is_specialization_of_sized_v<T, fragmented_vector>;

template<typename T>
concept is_std_unordered_map
  = ::detail::is_specialization_of_v<T, std::unordered_map>;

template<typename T>
concept is_absl_node_hash_set
  = ::detail::is_specialization_of_v<T, absl::node_hash_set>;

template<typename T>
concept is_absl_node_hash_map
  = ::detail::is_specialization_of_v<T, absl::node_hash_map>;

template<typename T>
inline constexpr auto const is_serde_compatible_v
  = is_envelope<T>
    || (std::is_scalar_v<T>  //
         && (!std::is_same_v<float, T> || std::numeric_limits<float>::is_iec559)
         && (!std::is_same_v<double, T> || std::numeric_limits<double>::is_iec559)
         && (!serde_is_enum_v<T> || sizeof(std::decay_t<T>) <= sizeof(serde_enum_serialized_t)))
    || reflection::is_std_vector<T>
    || reflection::is_rp_named_type<T>
    || reflection::is_ss_bool_class<T>
    || reflection::is_std_optional<T>
    || std::is_same_v<T, std::chrono::milliseconds>
    || std::is_same_v<T, iobuf>
    || std::is_same_v<T, ss::sstring>
    || std::is_same_v<T, bytes>
    || is_absl_btree_set<T>
    || is_absl_flat_hash_map<T>
    || is_absl_node_hash_set<T>
    || is_absl_node_hash_map<T>
    || is_std_unordered_map<T>
    || is_fragmented_vector<T> || reflection::is_tristate<T> || std::is_same_v<T, ss::net::inet_address>;

template<typename T>
inline constexpr auto const are_bytes_and_string_different = !(
  std::is_same_v<T, ss::sstring> && std::is_same_v<T, bytes>);

template<typename T>
void write(iobuf&, T);

template<typename T>
void write(iobuf& out, T t) {
    using Type = std::decay_t<T>;
    static_assert(are_bytes_and_string_different<Type>);
    static_assert(has_serde_write<Type> || is_serde_compatible_v<Type>);

    if constexpr (is_envelope<Type>) {
        write(out, Type::redpanda_serde_version);
        write(out, Type::redpanda_serde_compat_version);

        auto size_placeholder = out.reserve(sizeof(serde_size_t));

        auto checksum_placeholder = iobuf::placeholder{};
        if constexpr (is_checksum_envelope<Type>) {
            checksum_placeholder = out.reserve(sizeof(checksum_t));
        }

        auto const size_before = out.size_bytes();
        if constexpr (has_serde_write<Type>) {
            t.serde_write(out);
        } else {
            envelope_for_each_field(
              t, [&out](auto& f) { write(out, std::move(f)); });
        }

        auto const written_size = out.size_bytes() - size_before;
        if (unlikely(written_size > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception("envelope too big");
        }
        auto const size = ss::cpu_to_le(
          static_cast<serde_size_t>(written_size));
        size_placeholder.write(
          reinterpret_cast<char const*>(&size), sizeof(serde_size_t));

        if constexpr (is_checksum_envelope<Type>) {
            auto crc = crc::crc32c{};
            auto in = iobuf_const_parser{out};
            in.skip(size_before);
            in.consume(
              in.bytes_left(), [&crc](char const* src, size_t const n) {
                  crc.extend(src, n);
                  return ss::stop_iteration::no;
              });
            auto const checksum = ss::cpu_to_le(crc.value());
            static_assert(
              std::is_same_v<std::decay_t<decltype(checksum)>, checksum_t>);
            checksum_placeholder.write(
              reinterpret_cast<char const*>(&checksum), sizeof(checksum_t));
        }
    } else if constexpr (std::is_same_v<bool, Type>) {
        write<int8_t>(out, t);
    } else if constexpr (serde_is_enum_v<Type>) {
        auto const val = static_cast<std::underlying_type_t<Type>>(t);
        if (unlikely(
              val > std::numeric_limits<serde_enum_serialized_t>::max()
              || val < std::numeric_limits<serde_enum_serialized_t>::min())) {
            throw serde_exception{fmt_with_ctx(
              ssx::sformat,
              "serde: enum of type {} has value {} which is out of bounds for "
              "serde_enum_serialized_t",
              type_str<T>(),
              val)};
        }
        write(out, static_cast<serde_enum_serialized_t>(val));
    } else if constexpr (std::is_scalar_v<Type>) {
        if constexpr (sizeof(Type) == 1) {
            out.append(reinterpret_cast<char const*>(&t), sizeof(t));
        } else if constexpr (std::is_same_v<float, Type>) {
            auto const le_t = htole32(bit_cast<uint32_t>(t));
            static_assert(sizeof(le_t) == sizeof(Type));
            out.append(reinterpret_cast<char const*>(&le_t), sizeof(le_t));
        } else if constexpr (std::is_same_v<double, Type>) {
            auto const le_t = htole64(bit_cast<uint64_t>(t));
            static_assert(sizeof(le_t) == sizeof(Type));
            out.append(reinterpret_cast<char const*>(&le_t), sizeof(le_t));
        } else {
            auto const le_t = ss::cpu_to_le(t);
            static_assert(sizeof(le_t) == sizeof(Type));
            out.append(reinterpret_cast<char const*>(&le_t), sizeof(le_t));
        }
    } else if constexpr (reflection::is_std_vector<Type>) {
        if (unlikely(t.size() > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "serde: vector size {} exceeds serde_size_t",
              t.size()));
        }
        write(out, static_cast<serde_size_t>(t.size()));
        for (auto& el : t) {
            write(out, std::move(el));
        }
    } else if constexpr (reflection::is_rp_named_type<Type>) {
        return write(out, static_cast<typename Type::type>(t));
    } else if constexpr (reflection::is_ss_bool_class<Type>) {
        write(out, static_cast<int8_t>(bool(t)));
    } else if constexpr (std::is_same_v<Type, std::chrono::milliseconds>) {
        write<int64_t>(out, t.count());
    } else if constexpr (std::is_same_v<Type, iobuf>) {
        write<serde_size_t>(out, t.size_bytes());
        out.append(t.share(0, t.size_bytes()));
    } else if constexpr (std::is_same_v<Type, ss::sstring>) {
        write<serde_size_t>(out, t.size());
        out.append(t.data(), t.size());
    } else if constexpr (std::is_same_v<Type, bytes>) {
        write<serde_size_t>(out, t.size());
        out.append(t.data(), t.size());
    } else if constexpr (reflection::is_std_optional<Type>) {
        if (t) {
            write(out, true);
            write(out, std::move(t.value()));
        } else {
            write(out, false);
        }
    } else if constexpr (
      is_absl_node_hash_set<Type> || is_absl_btree_set<Type>) {
        if (unlikely(t.size() > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "serde: absl set size {} exceeds serde_size_t",
              t.size()));
        }
        write(out, static_cast<serde_size_t>(t.size()));
        for (auto& e : t) {
            write(out, e);
        }
    } else if constexpr (
      is_absl_node_hash_map<Type> || is_absl_flat_hash_map<Type>) {
        if (unlikely(t.size() > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "serde: absl map size {} exceeds serde_size_t",
              t.size()));
        }
        write(out, static_cast<serde_size_t>(t.size()));
        for (auto& v : t) {
            write(out, v.first);
            write(out, std::move(v.second));
        }
    } else if constexpr (is_std_unordered_map<Type>) {
        if (unlikely(t.size() > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "serde: std::unordered_map size {} exceeds serde_size_t",
              t.size()));
        }
        write(out, static_cast<serde_size_t>(t.size()));
        for (auto& v : t) {
            write(out, v.first);
            write(out, std::move(v.second));
        }
    } else if constexpr (is_fragmented_vector<Type>) {
        if (unlikely(t.size() > std::numeric_limits<serde_size_t>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "serde: fragmented vector size {} exceeds serde_size_t",
              t.size()));
        }
        write(out, static_cast<serde_size_t>(t.size()));
        for (auto& el : t) {
            write(out, std::move(el));
        }
    } else if constexpr (reflection::is_tristate<T>) {
        if (t.is_disabled()) {
            write<int8_t>(out, -1);
        } else if (!t.has_value()) {
            write<int8_t>(out, 0);
        } else {
            write<int8_t>(out, 1);
            write(out, std::move(t.value()));
        }
    } else if constexpr (std::is_same_v<T, ss::net::inet_address>) {
        iobuf address_bytes;

        // NOLINTNEXTLINE
        address_bytes.append((const char*)t.data(), t.size());

        write(out, t.is_ipv4());
        write(out, std::move(address_bytes));
    }
}

template<typename T>
std::decay_t<T> read_nested(iobuf_parser&, std::size_t bytes_left_limit);

template<typename T>
std::decay_t<T> read(iobuf_parser& in) {
    auto ret = read_nested<T>(in, 0U);
    if (unlikely(in.bytes_left() != 0)) {
        throw serde_exception{fmt_with_ctx(
          ssx::sformat,
          "serde: not all bytes consumed after read<{}>(), bytes_left={}",
          type_str<T>(),
          in.bytes_left())};
    }
    return ret;
}

template<typename T>
header read_header(iobuf_parser& in, std::size_t const bytes_left_limit) {
    using Type = std::decay_t<T>;

    auto const version = read_nested<version_t>(in, bytes_left_limit);
    auto const compat_version = read_nested<version_t>(in, bytes_left_limit);
    auto const size = read_nested<serde_size_t>(in, bytes_left_limit);

    auto checksum = checksum_t{};
    if constexpr (is_checksum_envelope<T>) {
        checksum = read_nested<checksum_t>(in, bytes_left_limit);
    }

    if (unlikely(in.bytes_left() < size)) {
        throw serde_exception(fmt_with_ctx(
          ssx::sformat,
          "bytes_left={}, size={}",
          in.bytes_left(),
          static_cast<int>(size)));
    }

    if (unlikely(in.bytes_left() - size < bytes_left_limit)) {
        throw serde_exception(fmt_with_ctx(
          ssx::sformat,
          "envelope does not fit into bytes left: bytes_left={}, size={}, "
          "bytes_left_limit={}",
          in.bytes_left(),
          static_cast<int>(size),
          bytes_left_limit));
    }

    if (unlikely(compat_version > Type::redpanda_serde_version)) {
        throw serde_exception(fmt_with_ctx(
          ssx::sformat,
          "read {}: compat_version={} > {}::version={}",
          type_str<Type>(),
          static_cast<int>(compat_version),
          type_str<T>(),
          static_cast<int>(Type::redpanda_serde_version)));
    }

    if (unlikely(version < Type::redpanda_serde_compat_version)) {
        throw serde_exception(fmt_with_ctx(
          ssx::sformat,
          "read {}: version={} < {}::compat_version={}",
          type_str<Type>(),
          static_cast<int>(version),
          type_str<T>(),
          static_cast<int>(Type::redpanda_serde_compat_version)));
    }

    return header{
      ._version = version,
      ._compat_version = compat_version,
      ._bytes_left_limit = in.bytes_left() - size,
      ._checksum = checksum};
}

template<typename T>
void read_nested(iobuf_parser& in, T& t, std::size_t const bytes_left_limit) {
    using Type = std::decay_t<T>;
    static_assert(are_bytes_and_string_different<Type>);
    static_assert(has_serde_read<T> || is_serde_compatible_v<Type>);

    if constexpr (is_envelope<Type>) {
        auto const h = read_header<Type>(in, bytes_left_limit);

        if constexpr (is_checksum_envelope<Type>) {
            auto const shared = in.share(in.bytes_left() - h._bytes_left_limit);
            auto read_only_in = iobuf_const_parser{shared};
            auto crc = crc::crc32c{};
            read_only_in.consume(
              read_only_in.bytes_left(),
              [&crc](char const* src, size_t const n) {
                  crc.extend(src, n);
                  return ss::stop_iteration::no;
              });
            if (unlikely(crc.value() != h._checksum)) {
                throw serde_exception(fmt_with_ctx(
                  ssx::sformat,
                  "serde: envelope {} (ends at bytes_left={}) has bad "
                  "checksum: stored={}, actual={}",
                  type_str<Type>(),
                  h._bytes_left_limit,
                  h._checksum,
                  crc.value()));
            }
        }

        if constexpr (has_serde_read<Type>) {
            t.serde_read(in, h);
        } else {
            envelope_for_each_field(t, [&](auto& f) {
                using FieldType = std::decay_t<decltype(f)>;
                if (h._bytes_left_limit == in.bytes_left()) {
                    return false;
                }
                if (unlikely(in.bytes_left() < h._bytes_left_limit)) {
                    throw serde_exception(fmt_with_ctx(
                      ssx::sformat,
                      "field spill over in {}, field type {}: envelope_end={}, "
                      "in.bytes_left()={}",
                      type_str<Type>(),
                      type_str<FieldType>(),
                      h._bytes_left_limit,
                      in.bytes_left()));
                }
                f = read_nested<FieldType>(in, bytes_left_limit);
                return true;
            });
        }
        if (in.bytes_left() > h._bytes_left_limit) {
            in.skip(in.bytes_left() - h._bytes_left_limit);
        }
    } else if constexpr (std::is_same_v<Type, bool>) {
        t = read_nested<int8_t>(in, bytes_left_limit) != 0;
    } else if constexpr (serde_is_enum_v<Type>) {
        auto const val = read_nested<serde_enum_serialized_t>(
          in, bytes_left_limit);
        if (unlikely(
              val > std::numeric_limits<std::underlying_type_t<Type>>::max())) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "enum value {} too large for {}",
              val,
              type_str<Type>()));
        }
        t = static_cast<Type>(val);
    } else if constexpr (std::is_scalar_v<Type>) {
        if (unlikely(in.bytes_left() < sizeof(Type))) {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "reading type {} of size {}: {} bytes left",
              type_str<Type>(),
              sizeof(Type),
              in.bytes_left()));
        }

        if constexpr (sizeof(Type) == 1) {
            t = in.consume_type<Type>();
        } else if constexpr (std::is_same_v<float, Type>) {
            t = bit_cast<float>(le32toh(in.consume_type<uint32_t>()));
        } else if constexpr (std::is_same_v<double, Type>) {
            t = bit_cast<double>(le64toh(in.consume_type<uint64_t>()));
        } else {
            t = ss::le_to_cpu(in.consume_type<Type>());
        }
    } else if constexpr (reflection::is_std_vector<Type>) {
        using value_type = typename Type::value_type;
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        t.reserve(size);
        for (auto i = 0U; i < size; ++i) {
            t.push_back(read_nested<value_type>(in, bytes_left_limit));
        }
    } else if constexpr (reflection::is_rp_named_type<Type>) {
        t = Type{read_nested<typename Type::type>(in, bytes_left_limit)};
    } else if constexpr (reflection::is_ss_bool_class<Type>) {
        t = Type{read_nested<int8_t>(in, bytes_left_limit) != 0};
    } else if constexpr (std::is_same_v<Type, std::chrono::milliseconds>) {
        t = std::chrono::milliseconds{
          read_nested<int64_t>(in, bytes_left_limit)};
    } else if constexpr (std::is_same_v<Type, iobuf>) {
        t = in.share(read_nested<serde_size_t>(in, bytes_left_limit));
    } else if constexpr (std::is_same_v<Type, ss::sstring>) {
        auto str = ss::uninitialized_string(
          read_nested<serde_size_t>(in, bytes_left_limit));
        in.consume_to(str.size(), str.begin());
        t = str;
    } else if constexpr (std::is_same_v<Type, bytes>) {
        auto str = ss::uninitialized_string<bytes>(
          read_nested<serde_size_t>(in, bytes_left_limit));
        in.consume_to(str.size(), str.begin());
        t = str;
    } else if constexpr (reflection::is_std_optional<Type>) {
        t = read_nested<bool>(in, bytes_left_limit)
              ? Type{read_nested<typename Type::value_type>(
                in, bytes_left_limit)}
              : std::nullopt;
    } else if constexpr (is_absl_node_hash_set<Type>) {
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        t.reserve(size);
        for (auto i = 0U; i < size; ++i) {
            auto elem = read_nested<typename Type::key_type>(
              in, bytes_left_limit);
            t.emplace(std::move(elem));
        }
    } else if constexpr (is_absl_btree_set<Type>) {
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        for (auto i = 0U; i < size; ++i) {
            auto elem = read_nested<typename Type::key_type>(
              in, bytes_left_limit);
            t.emplace(std::move(elem));
        }
    } else if constexpr (
      is_absl_node_hash_map<Type> || is_absl_flat_hash_map<Type>) {
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        t.reserve(size);
        for (auto i = 0U; i < size; ++i) {
            auto key = read_nested<typename Type::key_type>(
              in, bytes_left_limit);
            auto value = read_nested<typename Type::mapped_type>(
              in, bytes_left_limit);
            t.emplace(std::move(key), std::move(value));
        }
    } else if constexpr (is_std_unordered_map<Type>) {
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        t.reserve(size);
        for (auto i = 0U; i < size; ++i) {
            auto key = read_nested<typename Type::key_type>(
              in, bytes_left_limit);
            auto value = read_nested<typename Type::mapped_type>(
              in, bytes_left_limit);
            t.emplace(std::move(key), std::move(value));
        }
    } else if constexpr (is_fragmented_vector<Type>) {
        using value_type = typename Type::value_type;
        const auto size = read_nested<serde_size_t>(in, bytes_left_limit);
        for (auto i = 0U; i < size; ++i) {
            t.push_back(read_nested<value_type>(in, bytes_left_limit));
        }
        t.shrink_to_fit();
    } else if constexpr (reflection::is_tristate<T>) {
        int8_t flag = read_nested<int8_t>(in, bytes_left_limit);
        if (flag == -1) {
            // disabled
            t = T{};
        } else if (flag == 0) {
            // empty
            t = T(std::nullopt);
        } else if (flag == 1) {
            t = T(read_nested<typename T::value_type>(in, bytes_left_limit));
        } else {
            throw serde_exception(fmt_with_ctx(
              ssx::sformat,
              "reading type {} of size {}: {} bytes left - unexpected tristate "
              "state flag: {}, expected states are -1,0,1",
              type_str<Type>(),
              sizeof(Type),
              in.bytes_left(),
              flag));
        }
    } else if constexpr (std::is_same_v<T, ss::net::inet_address>) {
        bool is_ipv4 = read_nested<bool>(in, bytes_left_limit);
        auto address_buf = read_nested<iobuf>(in, bytes_left_limit);
        auto address_bytes = iobuf_to_bytes(address_buf);
        if (is_ipv4) {
            ::in_addr addr{};
            if (unlikely(address_bytes.size() != sizeof(addr))) {
                throw serde_exception(fmt_with_ctx(
                  ssx::sformat,
                  "reading type {} of size {}: {} bytes left - unexpected ipv4 "
                  "address size, read: {}, expected: {}",
                  type_str<Type>(),
                  sizeof(Type),
                  in.bytes_left(),
                  address_bytes.size(),
                  sizeof(addr)));
            }

            std::memcpy(&addr, address_bytes.c_str(), sizeof(addr));
            t = ss::net::inet_address(addr);
        } else {
            ::in6_addr addr{};
            if (unlikely(address_bytes.size() != sizeof(addr))) {
                throw serde_exception(fmt_with_ctx(
                  ssx::sformat,
                  "reading type {} of size {}: {} bytes left - unexpected ipv6 "
                  "address size, read: {}, expected: {}",
                  type_str<Type>(),
                  sizeof(Type),
                  in.bytes_left(),
                  address_bytes.size(),
                  sizeof(addr)));
            }
            std::memcpy(&addr, address_bytes.c_str(), sizeof(addr));
            t = ss::net::inet_address(addr);
        }
    }
}

template<typename T>
std::decay_t<T>
read_nested(iobuf_parser& in, std::size_t const bytes_left_limit) {
    using Type = std::decay_t<T>;
    auto t = Type();
    read_nested(in, t, bytes_left_limit);
    return t;
}

template<typename T>
ss::future<std::decay_t<T>>
read_async_nested(iobuf_parser& in, size_t const bytes_left_limit) {
    using Type = std::decay_t<T>;
    if constexpr (has_serde_async_read<Type>) {
        auto const h = read_header<Type>(in, bytes_left_limit);
        return ss::do_with(Type{}, [&in, h](Type& t) {
            return t.serde_async_read(in, h).then(
              [&t]() { return std::move(t); });
        });
    } else {
        return ss::make_ready_future<std::decay_t<T>>(read<T>(in));
    }
}

template<typename T>
ss::future<std::decay_t<T>> read_async(iobuf_parser& in) {
    return read_async_nested<T>(in, 0).then([&](std::decay_t<T>&& t) {
        if (likely(in.bytes_left() == 0)) {
            return ss::make_ready_future<std::decay_t<T>>(std::move(t));
        } else {
            return ss::make_exception_future<std::decay_t<T>>(
              serde_exception{fmt_with_ctx(
                ssx::sformat,
                "serde: not all bytes consumed after read_async<{}>(), "
                "bytes_left={}",
                type_str<T>(),
                in.bytes_left())});
        }
    });
}

template<typename T>
ss::future<> write_async(iobuf& out, T t) {
    using Type = std::decay_t<T>;
    if constexpr (is_envelope<Type> && has_serde_async_write<Type>) {
        write(out, Type::redpanda_serde_version);
        write(out, Type::redpanda_serde_compat_version);

        auto size_placeholder = out.reserve(sizeof(serde_size_t));
        auto const size_before = out.size_bytes();

        return ss::do_with(
          std::move(t),
          [&out, size_before, size_placeholder = std::move(size_placeholder)](
            T& t) mutable {
              return t.serde_async_write(out).then(
                [&out,
                 size_before,
                 size_placeholder = std::move(size_placeholder)]() mutable {
                    auto const written_size = out.size_bytes() - size_before;
                    if (unlikely(
                          written_size
                          > std::numeric_limits<serde_size_t>::max())) {
                        throw serde_exception{"envelope too big"};
                    }
                    auto const size = ss::cpu_to_le(
                      static_cast<serde_size_t>(written_size));
                    size_placeholder.write(
                      reinterpret_cast<char const*>(&size),
                      sizeof(serde_size_t));

                    return ss::make_ready_future<>();
                });
          });
    } else {
        write(out, std::move(t));
        return ss::make_ready_future<>();
    }
}

inline version_t peek_version(iobuf_parser& in) {
    if (unlikely(in.bytes_left() < sizeof(serde::version_t))) {
        throw serde_exception{"cannot peek version"};
    }
    auto version_reader = iobuf_parser{in.peek(sizeof(serde::version_t))};
    return serde::read_nested<serde::version_t>(version_reader, 0);
}

template<typename T>
iobuf to_iobuf(T&& t) {
    iobuf b;
    write(b, std::forward<T>(t));
    return b;
}

template<typename T>
T from_iobuf(iobuf b) {
    auto in = iobuf_parser{std::move(b)};
    return read<T>(in);
}

} // namespace serde
