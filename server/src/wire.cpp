#include "wire.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cerrno>

namespace bh {
namespace {

// SPEC §5. Order matters: the index is the code.
const char* const kTable[] = {
    ":method",        // 0x01
    ":path",          // 0x02
    ":authority",     // 0x03
    "content-length", // 0x04
    "content-type",   // 0x05
    "date",           // 0x06
    "server",         // 0x07
    "user-agent",     // 0x08
    "accept",         // 0x09
    "last-modified",  // 0x0A
};
constexpr uint8_t kTableSize = 10;

} // namespace

const char* staticName(uint8_t code) {
    if (code == 0 || code > kTableSize) return nullptr;
    return kTable[code - 1];
}

uint8_t staticCode(const std::string& name) {
    for (uint8_t i = 0; i < kTableSize; ++i) {
        if (name == kTable[i]) return static_cast<uint8_t>(i + 1);
    }
    return 0;
}

void putVarint(std::vector<uint8_t>& out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

bool getVarint(const uint8_t* buf, size_t len, size_t& off, uint32_t& out) {
    uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {           // five octets is the cap (SPEC §5)
        if (off >= len) return false;
        uint8_t b = buf[off++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            if (value > 0xFFFFFFFFull) return false;
            out = static_cast<uint32_t>(value);
            return true;
        }
        shift += 7;
    }
    return false;                            // 6th continuation octet
}

void encodeHeaders(std::vector<uint8_t>& out, const std::vector<Header>& headers) {
    putVarint(out, static_cast<uint32_t>(headers.size()));
    for (const Header& h : headers) {
        uint8_t code = staticCode(h.name);
        out.push_back(code);                 // 0 means "literal name follows"
        if (code == 0) {
            putVarint(out, static_cast<uint32_t>(h.name.size()));
            out.insert(out.end(), h.name.begin(), h.name.end());
        }
        putVarint(out, static_cast<uint32_t>(h.value.size()));
        out.insert(out.end(), h.value.begin(), h.value.end());
    }
}

bool decodeHeaders(const uint8_t* buf, size_t len, size_t& off,
                   std::vector<Header>& out, int& skipped) {
    skipped = 0;
    uint32_t count = 0;
    if (!getVarint(buf, len, off, count)) return false;
    if (count > 256) return false;           // nobody sends 256 headers

    for (uint32_t i = 0; i < count; ++i) {
        if (off >= len) return false;
        uint8_t code = buf[off++];

        std::string name;
        bool known = true;
        if (code == 0x00) {
            uint32_t nlen = 0;
            if (!getVarint(buf, len, off, nlen)) return false;
            if (nlen == 0 || nlen > kMaxFieldLen || off + nlen > len) return false;
            name.assign(reinterpret_cast<const char*>(buf + off), nlen);
            off += nlen;
        } else if (const char* t = staticName(code)) {
            name = t;
        } else {
            // SPEC §7, second half: an unknown static-table index is a future
            // table entry. The value is still length-prefixed, so step over
            // this one field and keep decoding the rest of the block.
            known = false;
        }

        uint32_t vlen = 0;
        if (!getVarint(buf, len, off, vlen)) return false;
        if (vlen > kMaxFieldLen || off + vlen > len) return false;

        if (known) {
            out.push_back(Header{std::move(name),
                                 std::string(reinterpret_cast<const char*>(buf + off), vlen)});
        } else {
            ++skipped;
        }
        off += vlen;
    }
    return true;
}

bool readFull(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t got = ::read(fd, p, n);
        if (got == 0) return false;                       // peer closed
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

bool writeFull(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t put = ::write(fd, p, n);
        if (put <= 0) {
            if (put < 0 && errno == EINTR) continue;
            return false;
        }
        p += put;
        n -= static_cast<size_t>(put);
    }
    return true;
}

ReadResult readFrame(int fd, FrameHeader& hdr, std::vector<uint8_t>& payload) {
    uint8_t raw[kFrameHeaderLen];

    // Distinguish "clean close before a frame" from "died mid-frame": read the
    // first octet on its own, because only there is EOF legal.
    ssize_t first = ::read(fd, raw, 1);
    while (first < 0 && errno == EINTR) first = ::read(fd, raw, 1);
    if (first == 0) return ReadResult::kEof;
    if (first < 0) return ReadResult::kIoError;
    if (!readFull(fd, raw + 1, kFrameHeaderLen - 1)) return ReadResult::kIoError;

    hdr.length = (static_cast<uint32_t>(raw[0]) << 16) |
                 (static_cast<uint32_t>(raw[1]) << 8) |
                  static_cast<uint32_t>(raw[2]);
    hdr.type   = raw[3];
    hdr.flags  = raw[4];
    hdr.stream = (static_cast<uint32_t>(raw[5]) << 16) |
                 (static_cast<uint32_t>(raw[6]) << 8) |
                  static_cast<uint32_t>(raw[7]);

    // Not skippable (SPEC §7): in both cases Length cannot be trusted to point
    // at the next frame header, so there is nowhere safe to resynchronise.
    if (hdr.type == FT_RESERVED) return ReadResult::kMalformed;
    if (hdr.length > kMaxFramePayload) return ReadResult::kMalformed;

    payload.resize(hdr.length);
    if (hdr.length > 0 && !readFull(fd, payload.data(), hdr.length)) {
        return ReadResult::kIoError;
    }
    return ReadResult::kOk;
}

bool writeFrame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                const uint8_t* payload, size_t n) {
    if (n > kMaxFramePayload) return false;
    uint8_t raw[kFrameHeaderLen];
    raw[0] = static_cast<uint8_t>((n >> 16) & 0xFF);
    raw[1] = static_cast<uint8_t>((n >> 8) & 0xFF);
    raw[2] = static_cast<uint8_t>(n & 0xFF);
    raw[3] = type;
    raw[4] = flags;
    raw[5] = static_cast<uint8_t>((stream >> 16) & 0xFF);
    raw[6] = static_cast<uint8_t>((stream >> 8) & 0xFF);
    raw[7] = static_cast<uint8_t>(stream & 0xFF);

    if (!writeFull(fd, raw, kFrameHeaderLen)) return false;
    if (n > 0 && !writeFull(fd, payload, n)) return false;
    return true;
}

std::string hexdump(const uint8_t* data, size_t n, const char* indent) {
    std::string out;
    char line[128];
    for (size_t off = 0; off < n; off += 16) {
        out += indent;
        std::snprintf(line, sizeof line, "%08zx  ", off);
        out += line;
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) {
                std::snprintf(line, sizeof line, "%02x ", data[off + i]);
                out += line;
            } else {
                out += "   ";
            }
            if (i == 7) out += ' ';
        }
        out += " |";
        for (size_t i = 0; i < 16 && off + i < n; ++i) {
            uint8_t c = data[off + i];
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        out += "|\n";
    }
    return out;
}

} // namespace bh
