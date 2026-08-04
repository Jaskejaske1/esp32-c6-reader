# RSVP Serial Protocol v1

USB serial is the v1 transport. Physical cable access is the trust boundary.

Default settings:

```text
baud: 115200
encoding: UTF-8 for text commands and responses
```

## Response Frames

The reader may print boot/debug logs at any time. Clients must ignore all text
outside framed responses:

```text
RSVP/1
STATE playing=0;wpm=250;word=1;total=33;chapter=1;total_chapters=1
UPLOAD idle
.
```

Errors use the same frame:

```text
RSVP/1
ERR unknown command
.
```

## Text Commands

Send commands as UTF-8 lines ending in `\n`:

```text
STATUS
PLAY
PAUSE
WPM:250
WPM:+25
WPM:-25
SEEK 1500
SEEK_CHAP 3
```

Unknown commands return `ERR unknown command`.

## Upload

Upload sends one line command followed by ACKed raw chunks:

```text
UPLOAD <byteCount> <chunkSize>
```

If the reader accepts the upload, it replies:

```text
RSVP/1
UPLOAD READY
.
```

The client then sends exactly one raw chunk. After each non-final chunk, the
reader writes it to `/book.upload` and replies:

```text
RSVP/1
UPLOAD CONT <written>/<byteCount>
.
```

The client must wait for `UPLOAD CONT` before sending the next chunk. The final
chunk returns the normal metadata/state/upload frame after validation and
activation. This gives the ESP32-C6 real backpressure while it writes LittleFS.

The CLI defaults to 96-byte chunks with a 1 ms delay between chunks. Maximum
chunk size accepted by firmware and CLI is 240 bytes.

The reader writes upload bytes to `/book.upload`, never directly to
`/book.rsvp`. After all bytes arrive, it validates the file and atomically
activates it. Failed or timed-out uploads delete only `/book.upload`.

The firmware treats upload timeout as idle time. The v1 idle timeout is 120
seconds so large books can be written to LittleFS without false timeout errors.

After upload, clients should verify the returned `STATE total=<wordCount>`
matches the imported word count.

## Future Transports

Android should first reuse this protocol over USB OTG serial. If that is not
viable, the fallback is a tiny HTTP API with JSON/status/control/upload
endpoints, not a web screen.
