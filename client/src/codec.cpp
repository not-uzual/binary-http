#include "codec.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace bc {
namespace {

// SPEC §5 static table. Index + 1 is the wire code.
const char* const kStatic[] = {
    ":method", ":path", ":authority", "content-length", "content-type",
    "date", "server", "user-agent", "accept", "last-modified",
};
constexpr uint8_t kStaticCount = 10;

} // namespace

const char* typeName(uint8_t type) {
    switch (type) {
        case kReserved: return "RESERVED";
        case kRequest:  return "REQUEST";
        case kResponse: return "RESPONSE";
        case kData:     return "DATA";
        case kPing:     return "PING";
        case kPong:     return "PONG";
        default:        return "UNKNOWN";
    }
}

// ---- ByteWriter -------------------------------------------------------------

void ByteWriter::varint(uint32_t v) {
    while (v >= 0x80) {
        buf_.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
}

void ByteWriter::raw(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
}

// ---- ByteReader -------------------------------------------------------------

uint8_t ByteReader::u8() {
    if (failed_ || i_ >= n_) { failed_ = true; return 0; }
    return p_[i_++];
}

uint16_t ByteReader::u16() {
    uint8_t hi = u8(), lo = u8();
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint32_t ByteReader::varint() {
    uint64_t v = 0;
    int shift = 0;
    for (int octet = 0; octet < 5; ++octet) {   // SPEC §5: five octets, no more
        if (failed_ || i_ >= n_) { failed_ = true; return 0; }
        uint8_t b = p_[i_++];
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            if (v > 0xFFFFFFFFull) { failed_ = true; return 0; }
            return static_cast<uint32_t>(v);
        }
        shift += 7;
    }
    failed_ = true;
    return 0;
}

std::string ByteReader::take(uint32_t n) {
    if (failed_ || n > n_ - i_) { failed_ = true; return {}; }
    std::string s(reinterpret_cast<const char*>(p_ + i_), n);
    i_ += n;
    return s;
}

void ByteReader::skip(uint32_t n) {
    if (failed_ || n > n_ - i_) { failed_ = true; return; }
    i_ += n;
}

// ---- Headers ----------------------------------------------------------------

uint8_t Headers::codeFor(const std::string& name) {
    for (uint8_t i = 0; i < kStaticCount; ++i) {
        if (name == kStatic[i]) return static_cast<uint8_t>(i + 1);
    }
    return 0;
}

const char* Headers::nameFor(uint8_t code) {
    if (code == 0 || code > kStaticCount) return nullptr;
    return kStatic[code - 1];
}

void Headers::encode(ByteWriter& out, const std::vector<Field>& fields) {
    out.varint(static_cast<uint32_t>(fields.size()));
    for (const Field& f : fields) {
        uint8_t code = codeFor(f.name);
        out.u8(code);
        if (code == 0) {                    // literal name: length-prefix it
            out.varint(static_cast<uint32_t>(f.name.size()));
            out.str(f.name);
        }
        out.varint(static_cast<uint32_t>(f.value.size()));
        out.str(f.value);
    }
}

bool Headers::decode(ByteReader& in, std::vector<Field>& out, int& skipped) {
    skipped = 0;
    uint32_t count = in.varint();
    if (in.failed() || count > 256) return false;

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t code = in.u8();

        std::string name;
        bool known = true;
        if (code == 0) {
            uint32_t nlen = in.varint();
            if (in.failed() || nlen == 0 || nlen > kMaxField) return false;
            name = in.take(nlen);
        } else if (const char* known_name = nameFor(code)) {
            name = known_name;
        } else {
            // A static-table entry from a future version of the spec. We do
            // not know the name, but the value is length-prefixed, so the
            // rest of the block is still decodable. Skip exactly this field.
            known = false;
        }

        uint32_t vlen = in.varint();
        if (in.failed() || vlen > kMaxField) return false;

        if (known) {
            out.push_back(Field{std::move(name), in.take(vlen)});
        } else {
            in.skip(vlen);
            ++skipped;
        }
        if (in.failed()) return false;
    }
    return true;
}

// ---- Connection -------------------------------------------------------------

Connection::~Connection() {
    if (fd_ >= 0) ::close(fd_);
}

bool Connection::dial(const std::string& host, const std::string& port, std::string& err) {
    if (fd_ >= 0) {
        // Unreachable by construction, and that is the point: there is no
        // second connection in this program (SPEC §1).
        err = "internal: dial() called twice";
        return false;
    }

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* list = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &list);
    if (rc != 0) {
        err = "resolve " + host + ": " + ::gai_strerror(rc);
        return false;
    }

    std::string lastErr;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { lastErr = std::strerror(errno); continue; }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            fd_ = fd;
            break;
        }
        lastErr = std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(list);

    if (fd_ < 0) {
        err = "connect " + host + ":" + port + ": " +
              (lastErr.empty() ? "no address" : lastErr);
        return false;
    }
    return true;
}

bool Connection::readExactly(void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t got = ::read(fd_, p, n);
        if (got == 0) return false;
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

bool Connection::writeExactly(const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t put = ::write(fd_, p, n);
        if (put <= 0) {
            if (put < 0 && errno == EINTR) continue;
            return false;
        }
        p += put;
        n -= static_cast<size_t>(put);
    }
    return true;
}

bool Connection::sendPreface(std::string& err) {
    if (!writeExactly(kPreface, kPrefaceLen)) {
        err = "write preface: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool Connection::send(uint8_t type, uint8_t flags, uint32_t stream,
                      const std::vector<uint8_t>& payload, std::string& err) {
    if (payload.size() > kMaxPayload) {
        err = "frame payload exceeds the 1 MiB cap";
        return false;
    }
    uint8_t hdr[kHeaderLen];
    const uint32_t n = static_cast<uint32_t>(payload.size());
    hdr[0] = uint8_t(n >> 16);
    hdr[1] = uint8_t(n >> 8);
    hdr[2] = uint8_t(n);
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = uint8_t(stream >> 16);
    hdr[6] = uint8_t(stream >> 8);
    hdr[7] = uint8_t(stream);

    if (!writeExactly(hdr, kHeaderLen) ||
        (n > 0 && !writeExactly(payload.data(), n))) {
        err = "write frame: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool Connection::recv(Frame& frame, std::string& err) {
    err.clear();

    uint8_t hdr[kHeaderLen];
    ssize_t first = ::read(fd_, hdr, 1);
    while (first < 0 && errno == EINTR) first = ::read(fd_, hdr, 1);
    if (first == 0) return false;                       // clean close, err empty
    if (first < 0) { err = std::strerror(errno); return false; }
    if (!readExactly(hdr + 1, kHeaderLen - 1)) {
        err = "connection died inside a frame header";
        return false;
    }

    frame.length = (uint32_t(hdr[0]) << 16) | (uint32_t(hdr[1]) << 8) | uint32_t(hdr[2]);
    frame.type   = hdr[3];
    frame.flags  = hdr[4];
    frame.stream = (uint32_t(hdr[5]) << 16) | (uint32_t(hdr[6]) << 8) | uint32_t(hdr[7]);

    // Neither of these is skippable: Length cannot be trusted to point at the
    // next header, so there is no safe place to resynchronise (SPEC §7).
    if (frame.type == kReserved) {
        err = "peer sent reserved frame type 0x00";
        return false;
    }
    if (frame.length > kMaxPayload) {
        err = "peer announced a " + std::to_string(frame.length) +
              " octet frame, above the 1 MiB cap";
        return false;
    }

    frame.payload.assign(frame.length, 0);
    if (frame.length > 0 && !readExactly(frame.payload.data(), frame.length)) {
        err = "connection died inside a frame payload";
        return false;
    }
    return true;
}

// ---- hexdump ----------------------------------------------------------------

void hexdumpTo(FILE* out, const uint8_t* data, size_t n, const char* indent) {
    for (size_t off = 0; off < n; off += 16) {
        std::fprintf(out, "%s%08zx  ", indent, off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) std::fprintf(out, "%02x ", data[off + i]);
            else             std::fputs("   ", out);
            if (i == 7) std::fputc(' ', out);
        }
        std::fputs(" |", out);
        for (size_t i = 0; i < 16 && off + i < n; ++i) {
            uint8_t c = data[off + i];
            std::fputc((c >= 0x20 && c < 0x7F) ? c : '.', out);
        }
        std::fputs("|\n", out);
    }
}

} // namespace bc
