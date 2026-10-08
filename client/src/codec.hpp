// codec.hpp -- binary-http framing and header codec (client-side implementation).
//
// Written against SPEC.md only. It deliberately shares nothing with the
// server's codec, down to the shape of the API: the server uses free
// functions over raw buffers, this one uses reader/writer objects. Two
// independent readings of the same document that interoperate are the point
// of the exercise; a client that only works against its own server is an
// implementation, not a protocol.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace bc {

// ---- SPEC §1-§5 constants ---------------------------------------------------

inline constexpr char     kPreface[]      = "binary-http\n";
inline constexpr size_t   kPrefaceLen     = 8;
inline constexpr size_t   kHeaderLen      = 8;         // 24/8/8/24
inline constexpr uint32_t kMaxPayload     = 1u << 20;  // 1 MiB policy cap
inline constexpr uint32_t kMaxField       = 8192;

enum Type : uint8_t {
    kReserved = 0x00,
    kRequest  = 0x01,
    kResponse = 0x02,
    kData     = 0x03,
    kPing     = 0x04,
    kPong     = 0x05,
};

enum Flag : uint8_t { kEndMessage = 0x01 };

const char* typeName(uint8_t type);

struct Field {
    std::string name;
    std::string value;
};

struct Frame {
    uint32_t             length = 0;
    uint8_t              type   = 0;
    uint8_t              flags  = 0;
    uint32_t             stream = 0;
    std::vector<uint8_t> payload;

    bool endOfMessage() const { return (flags & kEndMessage) != 0; }
};

// ---- buffers ----------------------------------------------------------------

class ByteWriter {
  public:
    void u8(uint8_t v)  { buf_.push_back(v); }
    void u16(uint16_t v) { buf_.push_back(uint8_t(v >> 8)); buf_.push_back(uint8_t(v)); }
    void varint(uint32_t v);
    void raw(const void* p, size_t n);
    void str(const std::string& s) { raw(s.data(), s.size()); }

    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }

  private:
    std::vector<uint8_t> buf_;
};

// Every accessor is total: on underflow or a bad encoding the reader latches
// `failed` and keeps returning zeroes, so callers check once at the end
// instead of after every field.
class ByteReader {
  public:
    ByteReader(const uint8_t* data, size_t len) : p_(data), n_(len) {}

    uint8_t     u8();
    uint16_t    u16();
    uint32_t    varint();
    std::string take(uint32_t n);
    void        skip(uint32_t n);

    bool   failed() const { return failed_; }
    size_t left()   const { return failed_ ? 0 : n_ - i_; }
    void   fail()         { failed_ = true; }

  private:
    const uint8_t* p_;
    size_t         n_;
    size_t         i_ = 0;
    bool           failed_ = false;
};

// ---- header block (SPEC §5) -------------------------------------------------

class Headers {
  public:
    // Static table lookups; 0 / nullptr mean "not in the table".
    static uint8_t     codeFor(const std::string& name);
    static const char* nameFor(uint8_t code);

    static void encode(ByteWriter& out, const std::vector<Field>& fields);

    // Unknown name codes (0x0B-0xFF) are future table entries: the field is
    // stepped over and counted, not treated as an error. See SPEC §7.
    static bool decode(ByteReader& in, std::vector<Field>& out, int& skipped);
};

// ---- the one connection (SPEC §1) -------------------------------------------

// Opens exactly one socket for its whole lifetime. There is no reconnect path
// in this class, by construction: "and never open a second connection".
class Connection {
  public:
    ~Connection();

    bool dial(const std::string& host, const std::string& port, std::string& err);
    bool sendPreface(std::string& err);

    bool send(uint8_t type, uint8_t flags, uint32_t stream,
              const std::vector<uint8_t>& payload, std::string& err);

    // Returns false on EOF, i/o failure or an unskippable frame; `err` is
    // empty exactly when the peer closed cleanly at a frame boundary.
    bool recv(Frame& frame, std::string& err);

    bool connected() const { return fd_ >= 0; }

  private:
    bool readExactly(void* buf, size_t n);
    bool writeExactly(const void* buf, size_t n);

    int fd_ = -1;
};

// ---- diagnostics ------------------------------------------------------------

void hexdumpTo(FILE* out, const uint8_t* data, size_t n, const char* indent);

} // namespace bc
