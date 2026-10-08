// connection.hpp -- one binary-http connection, served start to finish.
#pragma once

#include <string>

namespace bh {

struct Options {
    std::string root       = ".";
    int         port       = 9000;
    bool        verbose    = false;  // -v : hexdump every frame
    bool        sendUnknown = false; // -x : emit an unregistered frame type
                                     //      before each response, to exercise
                                     //      the peer's skip path (SPEC §7)
};

// Owns `fd` and closes it. Runs until the peer closes or framing breaks.
void serveConnection(int fd, const Options& opt, const std::string& peer);

} // namespace bh
