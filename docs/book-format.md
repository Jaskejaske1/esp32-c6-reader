# RSVP Book Format v1

The ESP32-C6 stores one active book at `/book.rsvp`. Upload clients may create
the same bytes on Windows, Android, or any other serial-capable device.

All integer fields are little-endian. Text is UTF-8.

## Header

The file starts with a packed 108-byte header:

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `RSVP` |
| 4 | 2 | version | `1` |
| 6 | 2 | headerSize | `108` |
| 8 | 4 | bookId | Random nonzero uint32 |
| 12 | 4 | wordCount | Number of payload records, greater than zero |
| 16 | 4 | payloadBytes | Bytes after the header |
| 20 | 2 | defaultWpm | `50..800` |
| 22 | 2 | flags | `0` for v1 |
| 24 | 48 | title | UTF-8, NUL-padded, truncated on character boundary |
| 72 | 32 | author | UTF-8, NUL-padded, truncated on character boundary |
| 104 | 4 | payloadCrc32 | CRC-32 of the payload bytes |

Python `struct` format:

```text
<4sHHIIIHH48s32sI
```

## Payload

The payload is a stream of word records:

```text
[length: uint8][UTF-8 bytes...]
```

Rules:

- `length` must be `1..23`.
- `length` counts bytes, not code points.
- A record must contain complete UTF-8 bytes; importers must not split inside a
  multibyte character.
- The current Python importer emits at most 20 bytes per generated record, which
  leaves room for the firmware's NUL terminator and future safety margin.
- v1 records are words only. Chapter/title records are a future format bump.

The firmware validates:

- magic, version, header size, word count, and default WPM;
- file length equals header plus payload;
- every record length fits the reader buffer;
- record count equals `wordCount`;
- CRC-32 of the payload equals `payloadCrc32`.

## Importer Rules

For `.txt` and `.epub` conversion:

- split on whitespace;
- normalize smart quotes and en/em dashes to simple ASCII punctuation;
- preserve UTF-8 Latin text, including Dutch accents;
- generate a fresh random `bookId` for every import;
- store payload CRC separately from `bookId`.

EPUB parsing stays off-device. The ESP32-C6 only receives `.rsvp` bytes.

## Test Vector

Use this vector when implementing another converter.

Input:

```text
text: Café déjà vu.
title: Vector
author: Tester
wpm: 250
bookId: 0x12345678
```

Expected output:

```text
wordCount: 3
fileBytes: 125
payloadBytes: 17
payloadCrc32: 0xf0642562
sha256: 6f83d3cb006cc267035041c22b28ae1f714673f7a8d3b73adceb4b7b39ac81c5
payloadHex: 05436166c3a90664c3a96ac3a00376752e
```
