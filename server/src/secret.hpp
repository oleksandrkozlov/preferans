#pragma once

#include "common/logger.hpp"

#include <boost/algorithm/hex.hpp>
#include <sodium.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace pref {

inline auto hex2bytes(const std::string_view hex) -> std::string
{
    auto result = std::string{};
    boost::algorithm::unhex(hex, std::back_inserter(result));
    return result;
}

[[maybe_unused]] inline auto hashSecretHex(const std::string_view hex) -> std::string
{
    if (sodium_init() < 0) { throw std::runtime_error{"couldn't init sodium"}; }
    const auto bytes = hex2bytes(hex);
    auto result = std::string(crypto_pwhash_STRBYTES, '\0');
    if (crypto_pwhash_str(
            std::data(result),
            std::data(bytes),
            std::size(bytes),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE)
        != 0) {
        throw std::runtime_error{"couldn't hash password"};
    }
    return result;
}

[[nodiscard]] inline auto verifySecretHex(const std::string_view hex, const std::string_view hash) -> bool
{
    try {
        if (sodium_init() < 0) {
            PREF_WARN("error: couldn't init sodium");
            return false;
        }
        const auto bytes = hex2bytes(hex);
        return 0 == crypto_pwhash_str_verify(std::data(hash), std::data(bytes), std::size(bytes));
    } catch (const std::exception& error) {
        WARN_VAR(error);
    } catch (...) {
        PREF_WARN("error: unknown");
    }
    return false;
}

} // namespace pref
