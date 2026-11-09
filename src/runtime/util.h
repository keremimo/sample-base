// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <charconv>
#include <stdexcept>
#include <string>

// Setup checks. A failed check ends the app with "sample-base: <message>".
inline void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

// Reads all of `text` as an unsigned decimal number. Empty text, signs,
// spaces and trailing characters are rejected.
template <class Integer> bool parse_unsigned(const std::string& text, Integer& value) {
    if (text.empty()) return false;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
