/**
 * @file base64.hpp
 * @brief Base64 (RFC 4648 standard alphabet, padded), from FreeBSD's
 *        libc/net/base64.c (ISC / IBM licence; see base64.cpp).
 */
#pragma once
#include <string>

namespace dolphin {

/// @brief Base64 of @p data, padded with '='.
std::string base64Encode(const std::string& data);
/// @brief Decode @p text (whitespace ignored). False if it isn't valid base64.
bool base64Decode(const std::string& text, std::string& out);

} // namespace dolphin
