# ESP32-C6 RSVP Reader

This is a small, dumb RSVP reader: three buttons, one monochrome I2C OLED, one
active book, and USB serial upload/control. EPUB parsing stays off-device on
the PC or phone; the ESP32-C6 only reads the compact `.rsvp` file format.

The firmware streams `book.rsvp` from LittleFS and never loads the whole book
into RAM. A bookmark is saved in NVS while paused and every 40 words while
reading.

## Build a test book and flash it

Run from the PlatformIO project root in PowerShell:

```powershell
py tools/make_book.py examples/demo.txt data/book.rsvp --title "RSVP Demo" --author "Jasper"
pio run --target uploadfs
pio run --target upload
```

`uploadfs` overwrites the LittleFS partition. Use it only to provision or reset
the bundled filesystem. Normal book updates go through USB serial upload to
`/book.upload`, validate there, and then atomically activate `/book.rsvp`.

## Normal workflow

1. Build and upload the firmware with PlatformIO.
2. List serial ports with `uv --project tools run rsvp ports`.
3. Check the reader with `uv --project tools run rsvp --port COM5 status`.
4. Upload one book with `uv --project tools run rsvp --port COM5 upload book.epub`.
5. Use Yellow to play/pause, Green to increase WPM, and Red to decrease WPM.

The reader stores exactly one active book. A successful serial upload replaces
the old book and resets the bookmark to the first word. A failed upload deletes
only the temporary `/book.upload` file.

## Buttons

- Yellow short press: play / pause.
- Yellow 3-second hold while paused: show `USB READY`.
- Green: speed up.
- Red: speed down.

## Serial protocol

USB serial is the v1 transport. Physical cable access is the trust boundary.
The default baud rate is `115200`.

Text commands are UTF-8 lines:

```text
STATUS
PLAY
PAUSE
WPM:250
WPM:+25
WPM:-25
UPLOAD <byteCount>
```

Responses are framed so CLI tools can ignore boot/debug logs:

```text
RSVP/1
META v1|Title|Author|120000
STATE playing=0;wpm=250;word=33;total=120000
UPLOAD idle
.
```

See [docs/serial-protocol.md](docs/serial-protocol.md) and
[docs/book-format.md](docs/book-format.md) for client implementation details.

## CLI

Run from the repository root:

```powershell
uv --project tools run rsvp ports
uv --project tools run rsvp --port COM5 status
uv --project tools run rsvp --port COM5 play
uv --project tools run rsvp --port COM5 pause
uv --project tools run rsvp --port COM5 wpm 300
uv --project tools run rsvp --port COM5 wpm +25
uv --project tools run rsvp --port COM5 wpm -25
uv --project tools run rsvp inspect examples\utf8-dutch.txt --title "UTF8 Dutch"
uv --project tools run rsvp convert examples\utf8-dutch.txt C:\tmp\utf8-dutch.rsvp --title "UTF8 Dutch"
uv --project tools run rsvp --port COM5 upload examples\utf8-dutch.txt --title "UTF8 Dutch"
```

Port selection:

- `--port COM5` wins.
- `RSVP_PORT=COM5` is the fallback.
- Auto-detection is used only if exactly one likely USB serial device is found.

Upload accepts `.epub`, `.txt`, and prebuilt `.rsvp` files. EPUB and text are
converted locally to the compact `.rsvp` format before serial transfer,
preserving UTF-8 Latin text such as Dutch accents. `rsvp inspect` runs that same
conversion and validation locally without connecting to the reader. `rsvp
convert` writes those bytes to a local `.rsvp` file for inspection, archiving,
or Android/client compatibility testing.

Each local import gets a fresh random book ID so an old bookmark is never
applied to a newly imported book. The payload CRC is stored separately and is
used only to validate the uploaded bytes.

## Future Android transport

Phase 2 should reuse this protocol over Android USB OTG serial. If that is not
viable, the fallback is a tiny HTTP API with JSON/status/control/upload
endpoints, not a web page.

## Local checks

```powershell
pio run
uv --project tools run python -m unittest discover -s tools/tests -t tools
```

## Hardware checkpoint checklist

After a firmware upload, test in this order:

1. OLED boots and shows the current reader screen.
2. Yellow toggles play/pause.
3. Green/Red change WPM while reading or paused.
4. Holding Yellow while paused shows `USB READY`.
5. `uv --project tools run rsvp ports` lists the board port.
6. `uv --project tools run rsvp --port COM5 status` returns metadata/state.
7. `uv --project tools run rsvp --port COM5 upload examples\utf8-dutch.txt --title "UTF8 Dutch"` activates a new book.
8. Accented words display without broken bytes.
