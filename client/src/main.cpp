// bcurl -- a binary-http client.
//
//   ./bcurl -v localhost:9000/index.html
//
// Builds the binary request frame, reads the response, writes the body to
// stdout, hexdumps every frame under -v, and exits non-zero on 4xx/5xx.
// One connection, for the whole run.
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <string>
#include <vector>

#include "codec.hpp"
#include "url.hpp"

namespace {

constexpr char kUserAgent[] = "bcurl/1.0 (binary-http)";

// An unregistered frame type, sent only under --probe-unknown so the server's
// skip path (SPEC §7) can be exercised on demand.
constexpr uint8_t kProbeType = 0x7E;

struct Flags {
    bool        verbose  = false;
    bool        probe    = false;
    bool        ping     = false;
    std::string method   = "GET";
    std::string output;                 // empty = stdout
    std::vector<bc::Field> extra;       // -H name:value
};

void usage() {
    std::fputs(
        "usage: bcurl [options] <url> [more-paths...]\n"
        "\n"
        "  <url>          [bhttp://]host[:port][/path]   (port defaults to 9000)\n"
        "  more-paths     additional /paths fetched over the SAME connection\n"
        "\n"
        "options:\n"
        "  -v              hexdump and annotate every frame in both directions\n"
        "  -I              send HEAD instead of GET (headers only)\n"
        "  -X <method>     request method (GET or HEAD)\n"
        "  -H 'n: v'       add a request header (repeatable)\n"
        "  -o <file>       write the body to <file> instead of stdout\n"
        "  --ping          send a PING frame first and wait for the PONG\n"
        "  --probe-unknown send an unregistered frame type (0x7E) before the\n"
        "                  request, to check the server skips it cleanly\n"
        "  -h              this message\n"
        "\n"
        "exit status:\n"
        "  0  response status < 400\n"
        "  4  response status 4xx\n"
        "  5  response status 5xx\n"
        "  1  local, network or protocol error\n"
        "  2  bad usage\n", stderr);
}

// Dumps the frame as it appears on the wire: the 8 header octets followed by
// the payload, so offset 0x00 in the dump is offset 0 of the frame.
void traceFrame(const char* dir, const bc::Frame& f, bool skipped) {
    std::fprintf(stderr, "%s %s  type=0x%02x flags=0x%02x stream=%u length=%u%s\n",
                 dir, bc::typeName(f.type), f.type, f.flags, f.stream, f.length,
                 skipped ? "   [unknown type -- skipped]" : "");
    if (f.flags & bc::kEndMessage) std::fputs("    flags: END_MESSAGE\n", stderr);

    std::vector<uint8_t> wire;
    wire.reserve(bc::kHeaderLen + f.payload.size());
    wire.push_back(uint8_t(f.length >> 16));
    wire.push_back(uint8_t(f.length >> 8));
    wire.push_back(uint8_t(f.length));
    wire.push_back(f.type);
    wire.push_back(f.flags);
    wire.push_back(uint8_t(f.stream >> 16));
    wire.push_back(uint8_t(f.stream >> 8));
    wire.push_back(uint8_t(f.stream));

    size_t show = f.length > 256 ? 256 : f.length;
    wire.insert(wire.end(), f.payload.begin(), f.payload.begin() + show);

    bc::hexdumpTo(stderr, wire.data(), wire.size(), "    ");
    if (show < f.length) {
        std::fprintf(stderr, "    ... %u more octets\n", f.length - uint32_t(show));
    }
}

void traceSent(const char* what, uint8_t type, uint8_t flags, uint32_t stream,
               const std::vector<uint8_t>& payload) {
    bc::Frame f;
    f.type = type;
    f.flags = flags;
    f.stream = stream;
    f.length = uint32_t(payload.size());
    f.payload = payload;
    (void)what;
    traceFrame("-->", f, false);
}

// One request/response exchange on an already-open connection.
// Returns the response status, or -1 on a protocol/transport failure.
int exchange(bc::Connection& conn, const Flags& flags, const bc::Target& target,
             const std::string& path, uint32_t stream, FILE* bodyOut) {
    std::string err;

    if (flags.probe) {
        std::vector<uint8_t> junk = {'f', 'r', 'o', 'm', '-', 'v', '2'};
        if (flags.verbose) traceSent("probe", kProbeType, 0, stream, junk);
        if (!conn.send(kProbeType, 0, stream, junk, err)) {
            std::fprintf(stderr, "bcurl: %s\n", err.c_str());
            return -1;
        }
    }

    // ---- build the REQUEST frame (SPEC §5: pseudo-headers first) ----
    std::vector<bc::Field> fields = {
        {":method",    flags.method},
        {":path",      path},
        {":authority", target.authority},
        {"user-agent", kUserAgent},
        {"accept",     "*/*"},
    };
    for (const bc::Field& f : flags.extra) fields.push_back(f);

    bc::ByteWriter w;
    bc::Headers::encode(w, fields);

    if (flags.verbose) {
        std::fprintf(stderr, "> %s %s  (stream %u)\n", flags.method.c_str(),
                     path.c_str(), stream);
        for (const bc::Field& f : fields) {
            uint8_t code = bc::Headers::codeFor(f.name);
            std::fprintf(stderr, ">   [%s] %s: %s\n",
                         code ? ("idx " + std::to_string(code)).c_str() : "literal",
                         f.name.c_str(), f.value.c_str());
        }
        traceSent("request", bc::kRequest, bc::kEndMessage, stream, w.bytes());
    }

    // No request body, so END_MESSAGE rides on the REQUEST frame itself.
    if (!conn.send(bc::kRequest, bc::kEndMessage, stream, w.bytes(), err)) {
        std::fprintf(stderr, "bcurl: %s\n", err.c_str());
        return -1;
    }

    // ---- read the response message ----
    int  status      = -1;
    bool sawResponse = false;
    bool done        = false;
    unsigned long long bodyBytes = 0;

    while (!done) {
        bc::Frame f;
        if (!conn.recv(f, err)) {
            if (err.empty()) {
                std::fputs("bcurl: server closed the connection mid-response\n", stderr);
            } else {
                std::fprintf(stderr, "bcurl: %s\n", err.c_str());
            }
            return -1;
        }

        bool unknown = (f.type != bc::kRequest && f.type != bc::kResponse &&
                        f.type != bc::kData && f.type != bc::kPing &&
                        f.type != bc::kPong);
        if (flags.verbose) traceFrame("<--", f, unknown);

        switch (f.type) {
            case bc::kResponse: {
                if (sawResponse) {
                    std::fputs("bcurl: two RESPONSE frames in one message\n", stderr);
                    return -1;
                }
                sawResponse = true;

                bc::ByteReader r(f.payload.data(), f.payload.size());
                status = r.u16();
                std::vector<bc::Field> hs;
                int skipped = 0;
                if (!bc::Headers::decode(r, hs, skipped) || r.failed()) {
                    std::fputs("bcurl: malformed header block in RESPONSE\n", stderr);
                    return -1;
                }
                if (flags.verbose) {
                    std::fprintf(stderr, "< %d\n", status);
                    for (const bc::Field& h : hs) {
                        std::fprintf(stderr, "<   %s: %s\n", h.name.c_str(), h.value.c_str());
                    }
                    if (skipped > 0) {
                        std::fprintf(stderr,
                                     "<   [%d field(s) with unknown name codes, skipped]\n",
                                     skipped);
                    }
                }
                if (f.endOfMessage()) done = true;
                break;
            }

            case bc::kData: {
                if (!sawResponse) {
                    std::fputs("bcurl: DATA before RESPONSE\n", stderr);
                    return -1;
                }
                if (!f.payload.empty()) {
                    std::fwrite(f.payload.data(), 1, f.payload.size(), bodyOut);
                    bodyBytes += f.payload.size();
                }
                if (f.endOfMessage()) done = true;
                break;
            }

            case bc::kPing: {
                std::vector<uint8_t> echo = f.payload;
                if (flags.verbose) traceSent("pong", bc::kPong, 0, 0, echo);
                if (!conn.send(bc::kPong, 0, 0, echo, err)) {
                    std::fprintf(stderr, "bcurl: %s\n", err.c_str());
                    return -1;
                }
                break;
            }

            case bc::kPong:
                break;

            case bc::kRequest:
                std::fputs("bcurl: server sent a REQUEST frame\n", stderr);
                return -1;

            default:
                // ---------------------------------------------------------
                // SPEC §7. recv() already consumed exactly f.length octets,
                // so the stream is still aligned on a frame boundary. Drop
                // the payload, say nothing to the peer, read the next frame.
                // This is the whole of the forward-compatibility story.
                // ---------------------------------------------------------
                if (!flags.verbose) {
                    std::fprintf(stderr,
                                 "bcurl: skipped unknown frame type 0x%02x (%u octets)\n",
                                 f.type, f.length);
                }
                break;
        }
    }

    std::fflush(bodyOut);
    if (flags.verbose) {
        std::fprintf(stderr, "< [%llu body octets, connection still open]\n", bodyBytes);
    }
    return status;
}

int exitCodeFor(int status) {
    if (status < 0)    return 1;
    if (status >= 500) return 5;
    if (status >= 400) return 4;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    Flags flags;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bcurl: %s needs an argument\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "-v" || a == "--verbose")        flags.verbose = true;
        else if (a == "-I" || a == "--head")      flags.method = "HEAD";
        else if (a == "-X" || a == "--method")    flags.method = next("-X");
        else if (a == "-o" || a == "--output")    flags.output = next("-o");
        else if (a == "--ping")                   flags.ping = true;
        else if (a == "--probe-unknown")          flags.probe = true;
        else if (a == "-h" || a == "--help")      { usage(); return 0; }
        else if (a == "-H" || a == "--header") {
            std::string hv = next("-H");
            size_t colon = hv.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "bcurl: -H wants 'name: value', got '%s'\n", hv.c_str());
                return 2;
            }
            std::string name = hv.substr(0, colon);
            std::string value = hv.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
            flags.extra.push_back(bc::Field{name, value});
        }
        else if (!a.empty() && a[0] == '-' && a != "-") { usage(); return 2; }
        else positional.push_back(a);
    }

    if (positional.empty()) { usage(); return 2; }

    // Whether a method is allowed is the server's call, not ours: bserve
    // answers 405 and some other binary-http server may not. A client that
    // second-guesses the peer's policy is a client that cannot talk to the
    // next implementation of the spec.
    for (char& c : flags.method) c = char(std::toupper(static_cast<unsigned char>(c)));

    bc::Target target;
    std::string err;
    if (!bc::parseTarget(positional[0], target, err)) {
        std::fprintf(stderr, "bcurl: %s\n", err.c_str());
        return 2;
    }

    std::vector<std::string> paths{target.path};
    for (size_t i = 1; i < positional.size(); ++i) {
        std::string p = positional[i];
        if (p.empty() || p[0] != '/') p = "/" + p;
        paths.push_back(p);
    }

    FILE* bodyOut = stdout;
    if (!flags.output.empty()) {
        bodyOut = std::fopen(flags.output.c_str(), "wb");
        if (!bodyOut) {
            std::fprintf(stderr, "bcurl: cannot write '%s': %s\n",
                         flags.output.c_str(), std::strerror(errno));
            return 1;
        }
    }

    // ---- the one and only connection ----
    bc::Connection conn;
    if (!conn.dial(target.host, target.port, err)) {
        std::fprintf(stderr, "bcurl: %s\n", err.c_str());
        if (bodyOut != stdout) std::fclose(bodyOut);
        return 1;
    }
    if (flags.verbose) {
        std::fprintf(stderr, "* connected to %s:%s\n", target.host.c_str(),
                     target.port.c_str());
        std::fputs("--> PREFACE  \"binary-http\\n\"\n", stderr);
        bc::hexdumpTo(stderr, reinterpret_cast<const uint8_t*>(bc::kPreface),
                      bc::kPrefaceLen, "    ");
    }
    if (!conn.sendPreface(err)) {
        std::fprintf(stderr, "bcurl: %s\n", err.c_str());
        if (bodyOut != stdout) std::fclose(bodyOut);
        return 1;
    }

    int worst = 0;

    if (flags.ping) {
        std::vector<uint8_t> nonce = {'b', 'c', 'u', 'r', 'l', '!', '!', '!'};
        if (flags.verbose) traceSent("ping", bc::kPing, 0, 0, nonce);
        if (!conn.send(bc::kPing, 0, 0, nonce, err)) {
            std::fprintf(stderr, "bcurl: %s\n", err.c_str());
            if (bodyOut != stdout) std::fclose(bodyOut);
            return 1;
        }
        bc::Frame f;
        if (!conn.recv(f, err) || f.type != bc::kPong) {
            std::fputs("bcurl: no PONG came back\n", stderr);
            if (bodyOut != stdout) std::fclose(bodyOut);
            return 1;
        }
        if (flags.verbose) traceFrame("<--", f, false);
    }

    // Client stream IDs: odd, strictly increasing, from 1 (SPEC §2).
    uint32_t stream = 1;
    for (const std::string& path : paths) {
        int status = exchange(conn, flags, target, path, stream, bodyOut);
        int code = exitCodeFor(status);
        if (code > worst) worst = code;
        if (status < 0) break;
        stream += 2;
    }

    if (bodyOut != stdout) std::fclose(bodyOut);
    return worst;
}
