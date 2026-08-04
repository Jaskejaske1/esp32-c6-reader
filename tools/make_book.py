#!/usr/bin/env python3
"""Create the v1 streaming RSVP book file used by the ESP32-C6 reader."""

from __future__ import annotations

import argparse
import re
import struct
import unicodedata
import zlib
from pathlib import Path

MAGIC = b"RSVP"
VERSION = 1
HEADER_FORMAT = "<4sHHIIIHH48s32sI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAX_WORD_BYTES = 20


def fixed_utf8(value: str, length: int) -> bytes:
    encoded = value.encode("utf-8")[: length - 1]
    while encoded:
        try:
            encoded.decode("utf-8")
            break
        except UnicodeDecodeError:
            encoded = encoded[:-1]
    return encoded.ljust(length, b"\0")


def normalise_words(text: str) -> list[bytes]:
    result: list[bytes] = []
    # The current U8g2 font and on-device ORP logic are byte-oriented. Keep the
    # stored payload ASCII until the format gains explicit Unicode code points.
    text = (text.replace("’", "'").replace("‘", "'").replace("“", '"')
                .replace("”", '"').replace("–", "-").replace("—", "-"))
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    for token in re.findall(r"\S+", text):
        encoded = token.encode("utf-8")
        while len(encoded) > MAX_WORD_BYTES:
            result.append(encoded[: MAX_WORD_BYTES - 1] + b"-")
            encoded = encoded[MAX_WORD_BYTES - 1 :]
        if encoded:
            result.append(encoded)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="UTF-8 plain-text source")
    parser.add_argument("output", type=Path, help="output .rsvp path")
    parser.add_argument("--title", default=None)
    parser.add_argument("--author", default="Unknown")
    parser.add_argument("--wpm", type=int, default=250)
    args = parser.parse_args()
    if not 50 <= args.wpm <= 800:
        parser.error("--wpm must be between 50 and 800")

    words = normalise_words(args.input.read_text(encoding="utf-8"))
    if not words:
        parser.error("input contains no readable words")
    payload = b"".join(bytes([len(word)]) + word for word in words)
    title = args.title or args.input.stem.replace("_", " ").title()
    book_id = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, HEADER_SIZE, book_id, len(words),
        len(payload), args.wpm, 0, fixed_utf8(title, 48),
        fixed_utf8(args.author, 32), book_id,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + payload)
    print(f"Created {args.output}: {len(words)} words, {len(header) + len(payload)} bytes")


if __name__ == "__main__":
    main()
