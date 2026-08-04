#!/usr/bin/env python3
"""Create the v1 streaming RSVP book file used by the ESP32-C6 reader."""

from __future__ import annotations

import argparse
import re
import secrets
import struct
import zlib
from pathlib import Path

from epub_import import read_epub

MAGIC = b"RSVP"
VERSION = 1
HEADER_FORMAT = "<4sHHIIIHH48s32sI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MIN_WPM = 50
MAX_WPM = 800
MAX_GENERATED_WORD_BYTES = 20


def fresh_book_id(payload_crc32: int) -> int:
    while True:
        book_id = secrets.randbits(32)
        if book_id != 0 and book_id != payload_crc32:
            return book_id


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
    text = (
        text.replace("’", "'")
        .replace("‘", "'")
        .replace("“", '"')
        .replace("”", '"')
        .replace("–", "-")
        .replace("—", "-")
    )
    for token in re.findall(r"\S+", text):
        result.extend(split_word_record(token))
    return result


def split_word_record(token: str) -> list[bytes]:
    chunks: list[bytes] = []
    current = bytearray()

    for char in token:
        encoded = char.encode("utf-8")

        if len(encoded) > MAX_GENERATED_WORD_BYTES:
            continue

        if len(current) + len(encoded) > MAX_GENERATED_WORD_BYTES:
            chunks.append(bytes(current))
            current.clear()

        current.extend(encoded)

    if current:
        chunks.append(bytes(current))

    return chunks


def build_book_bytes(
    text: str,
    *,
    title: str,
    author: str = "Unknown",
    wpm: int = 250,
    book_id: int | None = None,
) -> tuple[bytes, int]:
    if not MIN_WPM <= wpm <= MAX_WPM:
        raise ValueError(f"wpm must be between {MIN_WPM} and {MAX_WPM}")

    words = normalise_words(text)
    if not words:
        raise ValueError("input contains no readable words")

    payload = b"".join(bytes([len(word)]) + word for word in words)
    payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    book_id = fresh_book_id(payload_crc32) if book_id is None else book_id
    header = struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, HEADER_SIZE, book_id, len(words),
        len(payload), wpm, 0, fixed_utf8(title, 48),
        fixed_utf8(author, 32), payload_crc32,
    )
    return header + payload, len(words)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="UTF-8 .txt or .epub source")
    parser.add_argument("output", type=Path, help="output .rsvp path")
    parser.add_argument("--title", default=None)
    parser.add_argument("--author", default="Unknown")
    parser.add_argument("--wpm", type=int, default=250)
    args = parser.parse_args()

    if args.input.suffix.lower() == ".epub":
        try:
            epub = read_epub(str(args.input))
        except ValueError as exc:
            parser.error(str(exc))

        source_text = epub.text
        title = args.title or epub.title
        author = args.author if args.author != "Unknown" else epub.author
    else:
        source_text = args.input.read_text(encoding="utf-8")
        title = args.title or args.input.stem.replace("_", " ").title()
        author = args.author

    try:
        book, word_count = build_book_bytes(
            source_text,
            title=title,
            author=author,
            wpm=args.wpm,
        )
    except ValueError as exc:
        parser.error(str(exc))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(book)
    print(f"Created {args.output}: {word_count} words, {len(book)} bytes")


if __name__ == "__main__":
    main()
