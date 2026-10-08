# `bcurl` — The Client

```
$ make
$ ./bcurl -v localhost:9000/index.html
```

A binary-http client. Builds the binary request frame, reads the response, writes
the body to stdout, `-v` hexdumps every frame, exits non-zero on 4xx/5xx —
and never opens a second connection.

Full options, exit codes and `-v` output: [`../RUNNING.md`](../RUNNING.md).
The protocol: [`../SPEC.md`](../SPEC.md).

## Layout

| file | what it is |
|---|---|
| `src/main.cpp` | argument parsing, the request/response exchange, tracing, exit codes |
| `src/codec.hpp/.cpp` | the codec — `ByteWriter`/`ByteReader`, `Headers`, `Connection`, hexdump |
| `src/url.hpp/.cpp` | `[bhttp://]host[:port][/path]`, including bracketed IPv6 literals |

`src/codec.*` is this program's own reading of the spec, written against
`SPEC.md` and nothing else. The server's codec is a separate reading in a
separate style — free functions over raw buffers there, reader/writer objects
here. They have never shared a line, which is the only way to find out
whether the spec is actually a spec.

## The parts worth reading

* **`main.cpp`, the `default:` case of the response loop** — the
  unknown-frame rule, SPEC §7. `recv()` consumed exactly `length` octets, so
  the stream is still aligned; the payload is dropped and the next frame is
  read. The peer is told nothing.
* **`codec.cpp`, `Headers::decode()`** — the same rule for an unknown header
  name code: skip that field, keep the block.
* **`codec.hpp`, `class Connection`** — "never open a second connection"
  enforced by construction rather than by discipline. One `fd_`, no reconnect
  path, and `dial()` refuses a second call.
* **`codec.cpp`, `class ByteReader`** — every accessor is total. On underflow
  or a bad encoding the reader latches `failed_` and returns zeroes, so
  callers check once at the end instead of after every field. A decoder for
  bytes from the network is mostly a question of what it does when the bytes
  are wrong.
