#include "url.hpp"

#include <cstdlib>

namespace bc {
namespace {

constexpr char kDefaultPort[] = "9000";

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 5) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

} // namespace

bool parseTarget(const std::string& text, Target& out, std::string& err) {
    std::string rest = text;

    const std::string scheme = "bhttp://";
    if (rest.rfind(scheme, 0) == 0) {
        rest = rest.substr(scheme.size());
    } else if (rest.rfind("http://", 0) == 0) {
        // Convenience only; the bytes on the wire are binary-http either way.
        rest = rest.substr(7);
    }
    if (rest.empty()) { err = "empty target"; return false; }

    // Split authority from path at the first '/' outside an IPv6 bracket.
    size_t pathAt = std::string::npos;
    bool inBracket = false;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '[') inBracket = true;
        else if (rest[i] == ']') inBracket = false;
        else if (rest[i] == '/' && !inBracket) { pathAt = i; break; }
    }

    std::string authority = (pathAt == std::string::npos) ? rest : rest.substr(0, pathAt);
    out.path = (pathAt == std::string::npos) ? "/" : rest.substr(pathAt);
    if (authority.empty()) { err = "no host in '" + text + "'"; return false; }

    out.authority = authority;
    out.port = kDefaultPort;

    if (authority[0] == '[') {                       // [::1] or [::1]:9000
        size_t close = authority.find(']');
        if (close == std::string::npos) { err = "unterminated IPv6 literal"; return false; }
        out.host = authority.substr(1, close - 1);
        std::string tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail[0] != ':') { err = "junk after IPv6 literal"; return false; }
            out.port = tail.substr(1);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            out.host = authority;
        } else {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        }
    }

    if (out.host.empty())      { err = "empty host in '" + text + "'"; return false; }
    if (!allDigits(out.port))  { err = "bad port '" + out.port + "'"; return false; }
    if (std::atoi(out.port.c_str()) == 0) { err = "port 0 is not a port"; return false; }

    return true;
}

} // namespace bc
