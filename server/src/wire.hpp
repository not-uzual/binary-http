// wire.hpp -- binary-http framing and header codec (server-side implementation).
//
// This file is the server's own reading of SPEC.md. It shares no code with
// the client: the only thing that crosses between the two programs is the
// spec, so each side gets an independent decoder. If they interoperate, the
// spec is doing its job.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace bh {

// ---- constants from SPEC.md -------------------------------------------------

inline constexpr char     kPreface[]       = "binary-http\n";
inline constexpr size_t   kPrefaceLen      = 8;
inline constexpr size_t   kFrameHeaderLen  = 8;        // 24/8/8/24
inline constexpr uint32_t kMaxFramePayload = 1u << 20; // 1 MiB policy cap
inline constexpr uint32_t kMaxFieldLen     = 8192;     // name or value octets
inline constexpr size_t   kDataChunk       = 16384;    // body slice per DATA

enum FrameType : uint8_t {
    FT_RESERVED = 0x00,
    FT_REQUEST  = 0x01,
    FT_RESPONSE = 0x02,
    FT_DATA     = 0x03,
    FT_PING     = 0x04,
    FT_PONG     = 0x05,
};

enum FrameFlag : uint8_t {
    FL_END_MESSAGE = 0x01,
};

struct FrameHeader {
    uint32_t length = 0;
    uint8_t  type   = 0;
    uint8_t  flags  = 0;
    uint32_t stream = 0;
};

struct Header {
    std::string name;
    std::string value;
};

// Why a frame could not be read. kUnknownType is not an error -- see SPEC §7.
enum class ReadResult {
    kOk,
    kEof,         // clean close at a frame boundary
    kIoError,     // socket died mid-frame
    kMalformed,   // reserved type, or Length above the 1 MiB cap
};

// ---- static table (SPEC §5) -------------------------------------------------

// Returns the name for codes 1..10, or nullptr.
const char* staticName(uint8_t code);
// Returns 1..10 for a table name, or 0 if the name must be spelled out.
uint8_t staticCode(const std::string& name);

// ---- varint (unsigned LEB128, max 5 octets) ---------------------------------

void putVarint(std::vector<uint8_t>& out, uint32_t value);
bool getVarint(const uint8_t* buf, size_t len, size_t& off, uint32_t& out);

// ---- header block -----------------------------------------------------------

void encodeHeaders(std::vector<uint8_t>& out, const std::vector<Header>& headers);

// Decodes a header block. Fields whose name-code is an unknown static-table
// index (0x0B..0xFF) are skipped cleanly and counted in `skipped`; everything
// else that does not parse is a hard failure.
bool decodeHeaders(const uint8_t* buf, size_t len, size_t& off,
                   std::vector<Header>& out, int& skipped);

// ---- socket i/o -------------------------------------------------------------

bool readFull(int fd, void* buf, size_t n);
bool writeFull(int fd, const void* buf, size_t n);

ReadResult readFrame(int fd, FrameHeader& hdr, std::vector<uint8_t>& payload);
bool writeFrame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                const uint8_t* payload, size_t n);

// ---- diagnostics ------------------------------------------------------------

std::string hexdump(const uint8_t* data, size_t n, const char* indent = "  ");

} // namespace bh
