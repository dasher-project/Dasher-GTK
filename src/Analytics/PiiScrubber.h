#pragma once

#include <cstddef>
#include <string>

namespace analytics {

// Removes personally-identifying data from crash payloads before anything is
// written to disk or sent, per RFC 0009. Redacts home-directory user segments
// and email-shaped strings, and enforces the RFC's size caps.
class PiiScrubber {
  public:
    // Byte caps from RFC 0009: the saved stack trace, the engine log tail, and
    // the whole crash envelope respectively.
    static constexpr std::size_t kStackTraceCap = 16 * 1024;
    static constexpr std::size_t kEngineLogTailCap = 8 * 1024;
    static constexpr std::size_t kEnvelopeCap = 32 * 1024;

    // Replace home-directory user names (/home/<user>/, /Users/<user>/,
    // C:\Users\<user>\) with "<user>" and email addresses with "<email>".
    static std::string scrub(const std::string& input);

    // Truncate to at most max_bytes without splitting a UTF-8 sequence.
    static std::string truncate(const std::string& input, std::size_t max_bytes);
};

} // namespace analytics
