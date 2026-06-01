#include "fh6/playback_dsp.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fh6 {

namespace {

float parse_rg_float(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    try {
        return std::stof(std::string{s});
    } catch (...) {
        return std::numeric_limits<float>::quiet_NaN();
    }
}

bool ieq(char a, char b) noexcept {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

std::size_t find_ci(std::string_view hay, std::string_view needle,
                    std::size_t pos = 0) noexcept {
    if (needle.empty() || hay.size() < needle.size()) return std::string_view::npos;
    for (std::size_t i = pos; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (!ieq(hay[i + k], needle[k])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string_view::npos;
}

std::string_view extract_value(std::string_view buf, std::string_view key) noexcept {
    auto pos = find_ci(buf, key);
    if (pos == std::string_view::npos) return {};
    auto p = pos + key.size();
    if (p >= buf.size()) return {};
    if (buf[p] == '=' || buf[p] == '\0') ++p;
    auto start = p;
    while (p < buf.size() && buf[p] != '\0' && buf[p] != '\n' && buf[p] != '\r') ++p;
    return buf.substr(start, p - start);
}

} // namespace

void parse_replaygain_file(const std::filesystem::path& path, float& out_gain_db,
                           float& out_peak_lin) noexcept {
    std::ifstream f{path, std::ios::binary};
    if (!f) return;
    std::vector<char> buf(256 * 1024);
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<std::size_t>(f.gcount());
    if (n < 16) return;
    std::string_view sv{buf.data(), n};

    auto gain = extract_value(sv, "REPLAYGAIN_TRACK_GAIN");
    auto peak = extract_value(sv, "REPLAYGAIN_TRACK_PEAK");

    if (!gain.empty()) out_gain_db = parse_rg_float(gain);
    if (!peak.empty()) out_peak_lin = parse_rg_float(peak);
}

} // namespace fh6
