// url.hpp -- the small amount of URL parsing bcurl needs.
#pragma once

#include <string>

namespace bc {

struct Target {
    std::string host;       // "localhost", "127.0.0.1", "::1"
    std::string port;       // decimal string, defaults to 9000
    std::string authority;  // host:port as written, for the :authority header
    std::string path;       // origin-form, always begins with '/'
};

// Accepts  [bhttp://]host[:port][/path]  with bracketed IPv6 literals.
// Returns false and sets `err` on anything it cannot make sense of.
bool parseTarget(const std::string& text, Target& out, std::string& err);

} // namespace bc
