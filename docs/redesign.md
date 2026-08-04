# RSVP Reader Redesign

The reader is a small appliance, not a general book computer.

Its job is to show one word at a time, keep one bookmark, and accept one book
from a nearby client over USB serial.

## Product Rules

- Three colored physical buttons are the primary interface for low motoric dexterity.
- The ESP32-C6 stores exactly one active book.
- EPUB parsing and syllable-based hyphenation happen off-device on a PC or phone.
- The device reads only the `.rsvp` format (version 2 with Chapter Index Table).
- Words longer than 10 characters are split into natural hyphenated syllable cards (`weersomst-` -> `andigheden.`).
- The display shows an ORP focal letter box and per-chapter progress bar (`Ch X/Y`). Numeric word counts are removed to prevent cognitive fatigue.
- Uploads always go to a temporary file first (`/book.upload`).
- The active book is replaced only after validation succeeds.
- USB serial is the v1 transport.
- Android should reuse the serial protocol over USB OTG first.
- If serial is not viable on Android, fallback is a tiny HTTP API, not a web UI.
- Every import gets a fresh random book ID; payload CRC is only for validation.

## Buttons

- Yellow short press: play / pause.
- Yellow 3-second hold while paused: show `USB READY`.
- Green: speed up (+25 WPM).
- Red: speed down (-25 WPM).

## File Shape

Firmware modules should stay small and boring:

- `main.cpp`: setup, loop, wiring.
- `config.h`: pins, limits, file paths.
- `book_format.h`: `.rsvp` binary header.
- `book_store`: validate/read/activate `/book.rsvp`.
- `reader`: play/pause/WPM/bookmark/current word.
- `control`: apply text commands to reader state.
- `status_text`: render metadata/state/status response strings.
- `buttons`: debounce and button events.
- `upload_session`: temporary upload file writes and byte-count state.
- `display_ui`: OLED rendering.
- `serial_transport`: framed serial commands and exact-byte upload.

## Serial Direction

Keep the serial protocol tiny:

```text
STATUS
PLAY
PAUSE
WPM:300
WPM:+25
WPM:-25
UPLOAD <byteCount>
```

Responses are framed with `RSVP/1` and `.` so clients can ignore boot logs.
See `docs/serial-protocol.md`.
