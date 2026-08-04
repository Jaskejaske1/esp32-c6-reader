# ESP32-C6 RSVP Reader

An embedded, minimalistic e-reader designed around **Rapid Serial Visual Presentation (RSVP)**. Built for accessibility and ease of use, it is specifically tailored for individuals with **reading disabilities (dyslexia / visual processing)** and **motoric disabilities**.

The device features a high-contrast **monochrome 128x64 OLED display**, a **3-button color-coded physical interface**, **LittleFS flash streaming**, and an atomic **USB CDC serial transport**.

```text
+-----------------------------------+
| PLAYING                   250 WPM |   <-- Status & Speed Header
+-----------------------------------+
|                                   |
|               word                |   <-- RSVP Word Display & Focal ORP
|                                   |
+-----------------------------------+
| Ch 3/12 [=======================] |   <-- Chapter Counter & Per-Chapter Bar
+-----------------------------------+
```

---

## Key Accessibility & Design Principles

- **Simple 3-Button Color Interface**: Built for low motoric dexterity:
  - **Yellow Button** (GPIO 4): Short press = Play / Pause; 3-second hold while paused = `USB READY`.
  - **Green Button** (GPIO 5): Speed up (+25 WPM).
  - **Red Button** (GPIO 6): Speed down (-25 WPM).
- **Syllable-Based Word Splitting**: Words longer than 10 characters (e.g. `weersomstandigheden`) are split into natural hyphenated syllable cards (`weersom-` -> `standig-` -> `heden.`). Users never need to perform horizontal eye movements.
- **Optimal Recognition Point (ORP)**: Highlights the focal letter box of every word to accelerate visual word recognition and reduce eye fatigue.
- **Clutter-Free Display**: Removes numeric word counters (`33/120000`) to avoid cognitive strain while reading.
- **Per-Chapter Progress Bar**: Displays a progress bar showing completion percentage within the current chapter (`Ch X/Y`), resetting at chapter boundaries.
- **Calibrated WPM Timing**: Dynamic delays adjust for sentence endings (`.`, `!`, `?`), clauses (`,`, `;`), and compensate for I2C OLED display refresh overhead so configured WPM matches actual reading speed.

---

## Quick Start

### 1. Requirements & Dependencies
- **Hardware**: ESP32-C6 Microcontroller (e.g. ESP32-C6-DevKitM-1), SSD1309/SSD1306 128x64 OLED (I2C), 3 push buttons.
- **Software**: Python >= 3.14, [`uv`](https://github.com/astral-sh/uv), PlatformIO CLI.

### 2. Build & Flash Firmware
```powershell
pio run --target upload
```

### 3. List Ports & Check Reader Status
```powershell
uv --project tools run rsvp ports
uv --project tools run rsvp --port COM5 status
```

### 4. Upload an EPUB or TXT Book
```powershell
uv --project tools run rsvp --port COM5 upload de_acht_bergen.epub
```

*Note: Uploading streams books into `/book.upload` first, validates payload CRC32, and atomically activates `/book.rsvp`. Flash partition resetting via `pio run --target uploadfs` is only needed when pre-provisioning hardware.*

---

## Hardware Pin Wiring

| Component | Pin | Notes |
| :--- | :--- | :--- |
| **I2C SDA** | GPIO 8 | OLED Data line |
| **I2C SCL** | GPIO 10 | OLED Clock line |
| **Yellow Button** | GPIO 4 | Primary control (Internal Pullup) |
| **Green Button** | GPIO 5 | Speed Up / Accept (Internal Pullup) |
| **Red Button** | GPIO 6 | Speed Down / Reject (Internal Pullup) |
| **OLED Address** | `0x3C` | Standard I2C OLED address |

---

## Python CLI Reference (`tools/`)

The CLI handles off-device EPUB parsing, syllable hyphenation, `.rsvp` binary generation, and atomic serial transfer over USB CDC:

```powershell
# List available serial ports
uv --project tools run rsvp ports

# Display reader status and active book progress
uv --project tools run rsvp --port COM5 status

# Control playback
uv --project tools run rsvp --port COM5 play
uv --project tools run rsvp --port COM5 pause

# Adjust reading speed (WPM)
uv --project tools run rsvp --port COM5 wpm 300
uv --project tools run rsvp --port COM5 wpm +25
uv --project tools run rsvp --port COM5 wpm -25

# Inspect a book locally without uploading
uv --project tools run rsvp inspect examples\utf8-dutch.txt --title "UTF8 Dutch"

# Convert a .epub or .txt to local .rsvp format
uv --project tools run rsvp convert examples\utf8-dutch.txt C:\tmp\utf8-dutch.rsvp --title "UTF8 Dutch"

# Upload and activate a book on the reader
uv --project tools run rsvp --port COM5 upload de_acht_bergen.epub
```

---

## Custom `.rsvp` v2 Binary Format

Off-device tools emit a compact zero-copy binary format stored at `/book.rsvp` on LittleFS.

### Packed Header (108 Bytes)
```c
struct __attribute__((packed)) BookHeader {
  char magic[4];         // "RSVP"
  uint16_t version;      // 2 (or 1 for legacy)
  uint16_t headerSize;   // 108
  uint32_t bookId;       // Random uint32 (prevents bookmark mismatch)
  uint32_t wordCount;    // Total words in payload stream
  uint32_t payloadBytes; // Byte size of payload stream
  uint16_t defaultWpm;   // Default speed (50..800 WPM)
  uint16_t chapterCount; // Number of chapters (v2)
  char title[48];        // UTF-8 title, NUL-padded
  char author[32];       // UTF-8 author, NUL-padded
  uint32_t payloadCrc32; // CRC32 checksum of word payload
};
```

### Chapter Index Table (v2)
Located immediately after the header at offset 108:
```text
uint32_t chapterStartIndices[chapterCount]
```
Each entry contains the 0-based word index where that chapter starts.

### Payload Word Stream
Located at offset `108 + chapterCount * 4`:
```text
[length: uint8_t][UTF-8 string bytes...]
```
For full details, see [docs/book-format.md](docs/book-format.md).

---

## Serial Protocol (`RSVP/1`)

USB serial transport operates at `115200` baud. Responses are framed so CLI tools can ignore micro-controller boot and debug messages:

```text
RSVP/1
META v1|Title|Author|120000
STATE playing=0;wpm=250;word=33;total=120000;chapter=3;total_chapters=12
UPLOAD idle
.
```

For protocol details and chunked backpressure upload specs, see [docs/serial-protocol.md](docs/serial-protocol.md).

---

## Automated Doc Drift Prevention

To guarantee that documentation never falls out of sync with code, firmware constants, binary headers, or serial protocol frames, automated tests verify doc alignment:

```powershell
pio run
uv --project tools run python -m unittest discover -s tools/tests -t tools
```

Automated assertions in `test_rsvp_format.py` continuously test:
1. `docs/book-format.md` byte header fields, offsets, and test vectors against `make_book.py` and `config.h`.
2. `docs/serial-protocol.md` frame strings against `status_text.cpp` and `serial_transport.cpp`.
3. `README.md` code snippets and CLI flags against `rsvp_cli.py`.
