# Binary HTTP

**Version 1 · wire-stable**

binary-http carries HTTP-shaped request/response messages over a single TCP
connection as a stream of fixed-header binary frames. It is deliberately
small: one connection, one frame header, one header-compression trick, and
one rule that makes version 2 possible.

Everything multi-byte is **big-endian** (network byte order). Byte offsets
below are zero-based.

---

## 1. Connection

The client opens **one** TCP connection and reuses it for every request.
Before any frame, the client sends the 8-byte **preface**:

```
42 48 54 54 50 2F 31 0A      "binary-http\n"
```

The server reads exactly 8 bytes. On mismatch it replies with a `RESPONSE`
frame carrying status **505** on stream 0 and closes. The server sends no
preface of its own — its first byte is the first byte of a frame header.

The connection stays open after each exchange. Either side may close it; a
clean close is a TCP FIN at a frame boundary.

## 2. Frame header — 8 bytes

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------+---------------+---------------+---------------+
|                  Length (24)                  |    Type (8)   |
+---------------+---------------+---------------+---------------+
|   Flags (8)   |                Stream ID (24)                 |
+---------------+---------------+---------------+---------------+
|                    Payload (Length octets)                  ...
```

| off | size | field     | meaning                                       |
|-----|------|-----------|-----------------------------------------------|
| 0   | 24b  | Length    | payload octets that follow the 8-byte header  |
| 3   | 8b   | Type      | frame type, see §3                            |
| 4   | 8b   | Flags     | bit field, see §4                             |
| 5   | 24b  | Stream ID | 0 = connection-level; else the message's id   |

### Why these widths

* **Length = 24 bits.** HTTP/2 chose 24 because 16 is too small (a 64 KiB
  cap forces pointless fragmentation of ordinary web objects) and 32 is a
  denial-of-service invitation — a peer could announce a 4 GiB frame and
  make you decide what to do about it. 24 bits caps the *field* at 16 MiB
  while leaving the real limit to policy. binary-http keeps that and sets the
  policy limit at **1 MiB**: a receiver MUST reject a frame whose Length
  exceeds 1 048 576 with status 400. Bodies larger than that are sent as
  several `DATA` frames, which costs 8 bytes per megabyte — 0.0008%.
* **Type = 8 bits.** 256 frame types. HTTP/2 has used 10 in a decade.
* **Flags = 8 bits.** Per-type bit field. Byte-aligned so a receiver tests
  flags with a mask and no shifting.
* **Stream ID = 24 bits.** HTTP/2 spends 31 bits plus a reserved bit, and
  the reserved bit has never been used for anything. 24 bits gives 8.4
  million streams per connection; at 1000 requests/second a connection
  would have to stay open for 97 days to exhaust them. Dropping the dead
  reserved bit and seven bits of stream space buys the thing HTTP/2's
  9-byte header does not have: **the header is 8 bytes**, so it is a single
  aligned 64-bit load, and every payload begins on an 8-byte boundary when
  the previous one did. That is the whole trade, and it is why this header
  is 8/24/8/8/24 rather than 24/8/8/1/31.

Stream IDs are chosen by the client: **odd, strictly increasing, starting
at 1**. The server echoes the request's Stream ID on every frame of the
response. Stream ID 0 is reserved for connection-level frames (`PING`,
`PONG`, and errors raised before any request was parsed).

## 3. Frame types

| code | name       | payload                                    |
|------|------------|--------------------------------------------|
| 0x00 | —          | reserved, MUST NOT be sent (catches zeroed buffers) |
| 0x01 | `REQUEST`  | header block (§5)                          |
| 0x02 | `RESPONSE` | `u16` status code, then a header block     |
| 0x03 | `DATA`     | opaque body octets                         |
| 0x04 | `PING`     | 0–8 opaque octets, Stream ID 0             |
| 0x05 | `PONG`     | the `PING` payload verbatim, Stream ID 0   |
| 0x06–0xFF | —     | unregistered — see §7                      |

## 4. Flags

| bit  | name          | meaning                                        |
|------|---------------|------------------------------------------------|
| 0x01 | `END_MESSAGE` | this is the last frame of the message          |
| rest | —             | reserved; a receiver MUST ignore bits it does not know, not error on them |

A **message** is one `REQUEST` or `RESPONSE` frame followed by zero or more
`DATA` frames on the same Stream ID. The last frame of the message — and
only the last — carries `END_MESSAGE`. A body-less message is a single
frame with `END_MESSAGE` set.

## 5. Header block — the ten names

A header block is:

```
varint  count
count × {  u8 name-code
           [ varint name-len, name-len octets ]   // only when name-code == 0
           varint value-len, value-len octets  }
```

`varint` is unsigned LEB128: 7 payload bits per octet, high bit = "more
follows", at most 5 octets. A receiver MUST reject a longer encoding or a
value above the 1 MiB frame cap with status 400.

**Static table.** These are the ten names the two programs actually send,
so they are numbered instead of spelled. A name-code of `0x00` means the
name is spelled out; `0x01`–`0x0A` index this table:

| code | name              | code | name            |
|------|-------------------|------|-----------------|
| 0x01 | `:method`         | 0x06 | `date`          |
| 0x02 | `:path`           | 0x07 | `server`        |
| 0x03 | `:authority`      | 0x08 | `user-agent`    |
| 0x04 | `content-length`  | 0x09 | `accept`        |
| 0x05 | `content-type`    | 0x0A | `last-modified` |

That is HPACK's first two mechanisms and nothing else: an index into a
static table, and a length-prefixed literal for everything else. There is
no dynamic table, no Huffman coding, and no connection-wide encoder state —
which means a frame can be decoded on its own, and a dropped frame cannot
corrupt the next one.

Field names are lowercase ASCII. Values are opaque octets; they MUST NOT
contain `\r` or `\n`. A name longer than 8192 octets, or a value longer
than 8192 octets, is a malformed frame (400).

Names beginning with `:` are **pseudo-headers** and MUST precede ordinary
headers in the block. A `REQUEST` MUST carry `:method` and `:path`, and
SHOULD carry `:authority`. A `RESPONSE` carries no pseudo-headers; its
status is the `u16` at the front of the payload.

## 6. Semantics

**Request.** `:method` is `GET` or `HEAD`; anything else is **405**.
`:path` is an origin-form path, optionally followed by `?query`, and is
percent-decoded by the receiver. A path that does not begin with `/`, or
that still contains a `..` segment after decoding, is **403**.

**Response.** Status is a `u16`. Defined here:

| code | when |
|------|------|
| 200 | the bytes follow |
| 400 | malformed frame, bad preface framing, bad header block, frame too large |
| 403 | path escapes the document root |
| 404 | no such file under the root |
| 405 | method is not `GET` or `HEAD` |
| 414 | `:path` longer than 8192 octets |
| 500 | the file exists but could not be read |
| 501 | a registered frame type the receiver has not implemented |
| 505 | bad connection preface |

A 4xx/5xx response is a normal message with a short `text/html` body, and
the connection stays open — except after 400 or 505, where the receiver can
no longer trust the framing and MUST close after sending.

A `HEAD` response carries `content-length` for the file it would have sent,
and no `DATA` frames.

## 7. Forward compatibility — the rule you may not skip

> **A receiver that meets a frame Type it does not know MUST read the
> frame's Length, discard exactly that many octets, and continue reading
> the next frame header. It MUST NOT close the connection, MUST NOT reply
> with an error, and MUST NOT treat the frame as a protocol violation.**

This is the only reason a version 2 can exist. Because Length sits in the
header, ahead of Type, a receiver knows how far to jump before it knows
what it is jumping over. A v2 sender can interleave new frame types into a
v1 connection and a v1 receiver will step over them without noticing.

The same rule applies one level down, inside the header block: a name-code
in `0x0B`–`0xFF` is a future static-table entry. A v1 receiver does not
know the name, but the value is still length-prefixed, so it **MUST skip
that one field and keep decoding the rest of the block**. Forward
compatibility that stops at the frame boundary is not forward compatibility.

What a receiver may *not* skip: a frame whose Length exceeds the 1 MiB cap
(400, close), and type `0x00` (400, close). Neither is skippable, because
in both cases the Length cannot be trusted to point at the next header.

## 8. Worked example

A real `GET /hello.txt` for an 18-octet file, captured off the socket:

```
C: 42 48 54 54 50 2F 31 0A                      preface, 8 octets
C: 00 00 3C 01 01 00 00 01   <60-octet block>   REQUEST  #1, END_MESSAGE
S: 00 00 76 02 00 00 00 01   00 C8 <116 more>   RESPONSE #1, status 200
S: 00 00 12 03 01 00 00 01   <18 octets>        DATA     #1, END_MESSAGE
```

76 octets out, 152 back. The `RESPONSE` frame carries no `END_MESSAGE`, so
the client knows a body follows before it has parsed a single header.

A byte-by-byte annotation of a real exchange is in [`docs/HEXDUMP.md`](docs/HEXDUMP.md).
