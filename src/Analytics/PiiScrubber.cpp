#include "PiiScrubber.h"

#include <regex>

namespace analytics {

std::string PiiScrubber::scrub(const std::string& input) {
    // Home directory user segment: keep the prefix, redact the user name.
    //   /home/alice/... and /Users/alice/... -> /home/<user>/..., /Users/<user>/...
    static const std::regex kUnixHome(R"((/home/|/Users/)([^/\n\r]+))");
    //   C:\Users\alice\... -> C:\Users\<user>\...
    static const std::regex kWindowsHome(R"(([A-Za-z]:\\Users\\)([^\\/\n\r]+))");
    // Email-shaped strings.
    static const std::regex kEmail(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");

    std::string out = std::regex_replace(input, kUnixHome, "$1<user>");
    out = std::regex_replace(out, kWindowsHome, "$1<user>");
    out = std::regex_replace(out, kEmail, "<email>");
    return out;
}

std::string PiiScrubber::truncate(const std::string& input, std::size_t max_bytes) {
    if (input.size() <= max_bytes) return input;

    std::size_t cut = max_bytes;
    // Back off so we don't slice through a UTF-8 multi-byte sequence: trailing
    // continuation bytes have the form 10xxxxxx.
    while (cut > 0 && (static_cast<unsigned char>(input[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return input.substr(0, cut);
}

} // namespace analytics
