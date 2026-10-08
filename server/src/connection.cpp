#include "connection.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "files.hpp"
#include "wire.hpp"

namespace bh {
namespace {

constexpr char kServerName[] = "bserve/1.0 (binary-http)";

// An unregistered type. A conforming peer skips it (SPEC §7); we send it only
// under -x, to prove the peer actually does.
constexpr uint8_t kProbeType = 0x7F;

void log(const std::string& peer, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[%s] %s\n", peer.c_str(), msg);
}

void trace(const Options& opt, const char* dir, const FrameHeader& h,
           const uint8_t* payload) {
    if (!opt.verbose) return;
    const char* name = "UNKNOWN";
    switch (h.type) {
        case FT_REQUEST:  name = "REQUEST";  break;
        case FT_RESPONSE: name = "RESPONSE"; break;
        case FT_DATA:     name = "DATA";     break;
        case FT_PING:     name = "PING";     break;
        case FT_PONG:     name = "PONG";     break;
        default: break;
    }
    std::fprintf(stderr, "%s %s type=0x%02x flags=0x%02x stream=%u len=%u%s\n",
                 dir, name, h.type, h.flags, h.stream, h.length,
                 (h.flags & FL_END_MESSAGE) ? " END_MESSAGE" : "");

    // Dump the frame as it sits on the wire: 8 header octets, then payload.
    uint32_t show = h.length > 256 ? 256 : h.length;
    std::vector<uint8_t> wire;
    wire.reserve(kFrameHeaderLen + show);
    wire.push_back(static_cast<uint8_t>(h.length >> 16));
    wire.push_back(static_cast<uint8_t>(h.length >> 8));
    wire.push_back(static_cast<uint8_t>(h.length));
    wire.push_back(h.type);
    wire.push_back(h.flags);
    wire.push_back(static_cast<uint8_t>(h.stream >> 16));
    wire.push_back(static_cast<uint8_t>(h.stream >> 8));
    wire.push_back(static_cast<uint8_t>(h.stream));
    wire.insert(wire.end(), payload, payload + show);

    std::fputs(hexdump(wire.data(), wire.size()).c_str(), stderr);
    if (show < h.length) std::fprintf(stderr, "  ... %u more octets\n", h.length - show);
}

// Writes a frame and, under -v, traces exactly what went on the wire.
bool emit(int fd, const Options& opt, uint8_t type, uint8_t flags,
          uint32_t stream, const std::vector<uint8_t>& payload) {
    FrameHeader h;
    h.length = static_cast<uint32_t>(payload.size());
    h.type = type;
    h.flags = flags;
    h.stream = stream;
    trace(opt, "<--", h, payload.data());
    return writeFrame(fd, type, flags, stream, payload.data(), payload.size());
}

std::string errorBody(int status) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "<!doctype html><title>%d %s</title>\n"
                  "<h1>%d %s</h1>\n<p>bserve, speaking binary-http.</p>\n",
                  status, statusText(status), status, statusText(status));
    return buf;
}

// RESPONSE + (optionally) one DATA frame. Used for every error and for small
// bodies; large bodies go through sendFile() instead.
bool sendSimple(int fd, const Options& opt, uint32_t stream, int status,
                const std::string& body, const std::string& contentType,
                bool headOnly) {
    std::vector<Header> hs = {
        {"server", kServerName},
        {"date", nowDate()},
        {"content-type", contentType},
        {"content-length", std::to_string(body.size())},
    };

    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((status >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(status & 0xFF));
    encodeHeaders(payload, hs);

    bool noBody = headOnly || body.empty();
    if (!emit(fd, opt, FT_RESPONSE, noBody ? FL_END_MESSAGE : 0, stream, payload)) {
        return false;
    }
    if (noBody) return true;

    std::vector<uint8_t> data(body.begin(), body.end());
    return emit(fd, opt, FT_DATA, FL_END_MESSAGE, stream, data);
}

bool sendFile(int fd, const Options& opt, uint32_t stream, const Resolved& r,
              bool headOnly) {
    std::vector<uint8_t> body;
    if (!readFile(r.fsPath, body)) {
        return sendSimple(fd, opt, stream, 500, errorBody(500),
                          "text/html; charset=utf-8", false);
    }

    std::vector<Header> hs = {
        {"server", kServerName},
        {"date", nowDate()},
        {"content-type", r.contentType},
        {"content-length", std::to_string(body.size())},
        {"last-modified", r.lastModified},
    };

    std::vector<uint8_t> payload;
    payload.push_back(0x00);
    payload.push_back(0xC8);  // 200
    encodeHeaders(payload, hs);

    bool noBody = headOnly || body.empty();
    if (!emit(fd, opt, FT_RESPONSE, noBody ? FL_END_MESSAGE : 0, stream, payload)) {
        return false;
    }
    if (noBody) return true;

    // Slice into kDataChunk frames; only the last carries END_MESSAGE.
    for (size_t off = 0; off < body.size(); off += kDataChunk) {
        size_t n = body.size() - off;
        if (n > kDataChunk) n = kDataChunk;
        bool last = (off + n == body.size());
        std::vector<uint8_t> chunk(body.begin() + off, body.begin() + off + n);
        if (!emit(fd, opt, FT_DATA, last ? FL_END_MESSAGE : 0, stream, chunk)) {
            return false;
        }
    }
    return true;
}

const std::string* find(const std::vector<Header>& hs, const char* name) {
    for (const Header& h : hs) {
        if (h.name == name) return &h.value;
    }
    return nullptr;
}

// Returns false to close the connection.
bool handleRequest(int fd, const Options& opt, const std::string& peer,
                   const FrameHeader& hdr, const std::vector<uint8_t>& payload) {
    std::vector<Header> hs;
    size_t off = 0;
    int skipped = 0;
    if (!decodeHeaders(payload.data(), payload.size(), off, hs, skipped)) {
        log(peer, "400 malformed header block");
        sendSimple(fd, opt, hdr.stream, 400, errorBody(400),
                   "text/html; charset=utf-8", false);
        return false;  // framing is no longer trustworthy
    }
    if (skipped > 0) {
        log(peer, "skipped %d header field(s) with unknown name codes", skipped);
    }

    const std::string* method = find(hs, ":method");
    const std::string* path   = find(hs, ":path");
    if (!method || !path) {
        log(peer, "400 missing :method or :path");
        sendSimple(fd, opt, hdr.stream, 400, errorBody(400),
                   "text/html; charset=utf-8", false);
        return false;
    }

    bool isHead = (*method == "HEAD");
    if (*method != "GET" && !isHead) {
        log(peer, "405 %s %s", method->c_str(), path->c_str());
        return sendSimple(fd, opt, hdr.stream, 405, errorBody(405),
                          "text/html; charset=utf-8", false);
    }

    if (opt.sendUnknown) {
        // SPEC §7 in action: an unregistered type the peer must step over.
        std::vector<uint8_t> probe = {'v', '2', '-', 'o', 'n', 'l', 'y'};
        if (!emit(fd, opt, kProbeType, 0, hdr.stream, probe)) return false;
    }

    Resolved r = resolvePath(opt.root, *path);
    if (r.status != 200) {
        log(peer, "%d %s %s", r.status, method->c_str(), path->c_str());
        return sendSimple(fd, opt, hdr.stream, r.status, errorBody(r.status),
                          "text/html; charset=utf-8", isHead);
    }

    log(peer, "200 %s %s -> %s (%llu octets)", method->c_str(), path->c_str(),
        r.fsPath.c_str(), static_cast<unsigned long long>(r.size));
    return sendFile(fd, opt, hdr.stream, r, isHead);
}

} // namespace

void serveConnection(int fd, const Options& opt, const std::string& peer) {
    log(peer, "connection opened");

    char preface[kPrefaceLen];
    if (!readFull(fd, preface, kPrefaceLen)) {
        log(peer, "closed before preface");
        ::close(fd);
        return;
    }
    if (std::memcmp(preface, kPreface, kPrefaceLen) != 0) {
        log(peer, "505 bad preface");
        sendSimple(fd, opt, 0, 505, errorBody(505), "text/html; charset=utf-8", false);
        ::close(fd);
        return;
    }
    if (opt.verbose) {
        std::fprintf(stderr, "--> PREFACE\n%s",
                     hexdump(reinterpret_cast<const uint8_t*>(preface), kPrefaceLen).c_str());
    }

    bool open = true;
    while (open) {
        FrameHeader hdr;
        std::vector<uint8_t> payload;
        ReadResult rr = readFrame(fd, hdr, payload);

        if (rr == ReadResult::kEof)     { log(peer, "peer closed"); break; }
        if (rr == ReadResult::kIoError) { log(peer, "i/o error mid-frame"); break; }
        if (rr == ReadResult::kMalformed) {
            log(peer, "400 unskippable frame (reserved type or oversize length)");
            sendSimple(fd, opt, 0, 400, errorBody(400), "text/html; charset=utf-8", false);
            break;
        }

        trace(opt, "-->", hdr, payload.data());

        switch (hdr.type) {
            case FT_REQUEST:
                open = handleRequest(fd, opt, peer, hdr, payload);
                break;

            case FT_DATA:
                // A request body. We serve files; there is nothing to do with
                // it, but reading and dropping it keeps the stream in sync.
                break;

            case FT_PING:
                if (!emit(fd, opt, FT_PONG, 0, 0, payload)) open = false;
                break;

            case FT_PONG:
                break;

            case FT_RESPONSE:
                log(peer, "400 client sent a RESPONSE frame");
                sendSimple(fd, opt, hdr.stream, 400, errorBody(400),
                           "text/html; charset=utf-8", false);
                open = false;
                break;

            default:
                // ---------------------------------------------------------
                // SPEC §7. The one line you may not skip -- by skipping.
                // readFrame() already consumed exactly `hdr.length` octets,
                // so the next read lands on a frame header. No error, no
                // close: this is how a v2 sender talks past a v1 receiver.
                // ---------------------------------------------------------
                log(peer, "skipped unknown frame type 0x%02x (%u octets)",
                    hdr.type, hdr.length);
                break;
        }
    }

    log(peer, "connection closed");
    ::close(fd);
}

} // namespace bh
