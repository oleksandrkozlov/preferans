// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#pragma once

#include "common/common.hpp"

#include <botan/argon2fmt.h>
#include <botan/hash.h>
#include <botan/hex.h>
#include <botan/rng.h>
#include <botan/system_rng.h>
#include <botan/uuid.h>
#include <range/v3/all.hpp>

#include <concepts>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace pref {

template<typename T>
concept CompatibleCharByte = std::is_same_v<T, char> or std::is_same_v<T, unsigned char>;

template<typename T>
concept CharSequence = CompatibleCharByte<std::uint8_t>
    and std::ranges::contiguous_range<T>
    and std::same_as<std::remove_cv_t<std::ranges::range_value_t<T>>, char>;

[[nodiscard]] inline auto toBytes(CharSequence auto&& s) noexcept
{
    using T = std::remove_reference_t<decltype(s)>;
    using R = std::conditional_t<std::is_const_v<T>, const std::uint8_t, std::uint8_t>;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::span<R>{reinterpret_cast<R*>(std::data(s)), std::size(s)};
}

[[nodiscard]] inline auto generateUuid() -> std::string
{
    const auto uuid = Botan::UUID{Botan::system_rng()}.to_string();
    return uuid | ToLower | ToString;
}

[[nodiscard]] inline auto hex2bytes(const std::string_view hex) -> std::string
{
    auto result = std::string(std::size(hex) / 2, {});
    Botan::hex_decode(toBytes(result), hex, false);
    return result;
}

[[nodiscard]] inline auto bytes2hex(const std::string_view bytes) -> std::string
{
    return Botan::hex_encode(toBytes(bytes), false);
}

[[nodiscard]] inline auto hashPassword(const std::string_view password) -> std::string
{
    return Botan::argon2_generate_pwhash(std::data(password), std::size(password), Botan::system_rng(), 1, 65536, 2);
}

[[nodiscard]] inline auto verifyPassword(const std::string_view password, const std::string_view hash) -> bool
{
    return Botan::argon2_check_pwhash(std::data(password), std::size(password), hash);
}

[[nodiscard]] inline auto generateToken() -> std::string
{
    auto result = std::string(32, {});
    Botan::system_rng().randomize(toBytes(result));
    return result;
}

[[nodiscard]] inline auto hashToken(const std::string_view token) -> std::string
{
    const auto hash = Botan::HashFunction::create_or_throw("BLAKE2b(256)");
    auto out = std::string(32, {});
    hash->update(token);
    hash->final(toBytes(out));
    return bytes2hex(out);
}

} // namespace pref
