# `bserve` — The Server

```
$ make
$ ./bserve ./www 9000
```

A binary-http static file server. Accepts a TCP connection, reads one binary
request frame, maps the path to a file under a root, replies with status,
headers and the bytes. 404 if it is not there, 400 if the frame is
malformed — and it keeps the connection open.

Full options, logging format and behaviour: [`../RUNNING.md`](../RUNNING.md).
The protocol: [`../SPEC.md`](../SPEC.md).

## Layout

| file | what it is |
|---|---|
| `src/main.cpp` | argument parsing, listening socket, accept loop, one thread per connection |
| `src/connection.hpp/.cpp` | the per-connection state machine: preface, frame dispatch, request handling, response building |
| `src/wire.hpp/.cpp` | the codec — frame header, varints, header block, socket i/o, hexdump |
| `src/files.hpp/.cpp` | `:path` → file: percent-decoding, traversal rejection, directory index, MIME types, RFC 1123 dates |
| `www/` | a sample document root |

`src/wire.*` is this program's own reading of the spec. The client has its
own, in a deliberately different style, and the two share nothing — see the
note in the header of either file.

## The parts worth reading

* **`connection.cpp`, the `default:` case of the frame dispatch** — the
  unknown-frame rule, SPEC §7. `readFrame()` has already consumed exactly
  `Length` octets, so the next read lands on a frame header. Nothing is sent,
  nothing is closed.
* **`wire.cpp`, `decodeHeaders()`** — the same rule one level down. A name
  code in `0x0B`–`0xFF` is a future static-table entry; the value is still
  length-prefixed, so that one field is skipped and the rest of the block
  decodes normally.
* **`wire.cpp`, `readFrame()`** — the two things that are *not* skippable:
  reserved type `0x00` and a `Length` above the 1 MiB cap. In both cases
  `Length` cannot be trusted to point at the next header, so there is nowhere
  safe to resynchronise. Both are 400 and a close.
* **`files.cpp`, `escapesRoot()`** — traversal is rejected on the decoded
  path, before the filesystem is touched, so a symlink cannot widen the check.
