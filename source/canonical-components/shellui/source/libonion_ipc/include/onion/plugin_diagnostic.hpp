#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace onion::ipc {

inline constexpr std::string_view kPluginPending = "pending-return-to-game";
inline bool plugin_operation_pending(bool accepted, std::string_view detail) {
  return accepted && detail == kPluginPending;
}

// Diagnostics are plain text, not markup. Native Label.Text accepts them as
// text; callers generating XML must continue using the Page builder's escaping.
inline std::string plugin_diagnostic(std::string_view input) {
  constexpr size_t limit = 240;
  std::string out;
  out.reserve(limit);
  for (size_t pos = 0; pos < input.size() && out.size() < limit;) {
    const uint8_t first = static_cast<uint8_t>(input[pos]);
    size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                          : first >= 0xe0 && first <= 0xef ? 3
                          : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    uint32_t value = length == 1 ? first : first & ((1u << (7 - length)) - 1);
    bool valid = length && pos + length <= input.size();
    for (size_t i = 1; valid && i < length; ++i) {
      const auto byte = static_cast<uint8_t>(input[pos + i]);
      valid = (byte & 0xc0) == 0x80;
      value = (value << 6) | (byte & 0x3f);
    }
    valid = valid && (length < 2 || value >= 0x80) &&
            (length < 3 || value >= 0x800) && (length < 4 || value >= 0x10000) &&
            value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
    if (!valid) { out.push_back('?'); ++pos; continue; }
    pos += length;
    if (value < 0x20 || (value >= 0x7f && value <= 0x9f)) {
      if ((value == '\n' || value == '\r' || value == '\t') &&
          !out.empty() && out.back() != ' ') out.push_back(' ');
      continue;
    }
    if ((value >= 0x202a && value <= 0x202e) ||
        (value >= 0x2066 && value <= 0x2069)) continue;
    if (out.size() + length > limit) break;
    out.append(input.substr(pos - length, length));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

inline bool plugin_operation_result(bool success, std::string_view response,
                                    std::string *detail) {
  if (detail) *detail = success
      ? (response == kPluginPending ? std::string(kPluginPending) : std::string{})
      : plugin_diagnostic(response);
  return success;
}
} // namespace onion::ipc
