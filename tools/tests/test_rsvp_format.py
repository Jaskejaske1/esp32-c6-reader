from __future__ import annotations

import argparse
import hashlib
import io
import re
import struct
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

from epub_import import read_epub
from make_book import MAX_GENERATED_WORD_BYTES, build_book_bytes, normalise_words
import rsvp_cli
from rsvp_cli import (
    BOOK_HEADER_FORMAT,
    BOOK_HEADER_SIZE,
    DEFAULT_UPLOAD_CHUNK_SIZE,
    MAX_WPM,
    MAX_UPLOAD_CHUNK_SIZE,
    MAX_READER_WORD_BYTES,
    MIN_WPM,
    MIN_UPLOAD_CHUNK_SIZE,
    SerialReaderClient,
    build_upload_payload,
    convert_command,
    describe_upload_payload,
    extract_state_text,
    inspect_command,
    parse_state,
    serial_error_message,
    upload_command,
    validate_rsvp_book,
    write_file_atomic,
    wpm_control_command,
)


def unpack_header(data: bytes) -> tuple:
    return struct.unpack(BOOK_HEADER_FORMAT, data[:BOOK_HEADER_SIZE])


def payload_words(data: bytes) -> list[str]:
    payload = data[BOOK_HEADER_SIZE:]
    words: list[str] = []
    offset = 0

    while offset < len(payload):
        length = payload[offset]
        offset += 1
        words.append(payload[offset : offset + length].decode("utf-8"))
        offset += length

    return words


def upload_args(path: Path, **overrides) -> argparse.Namespace:
    values = {
        "path": str(path),
        "title": None,
        "author": "Unknown",
        "wpm": 250,
        "chunk_size": DEFAULT_UPLOAD_CHUNK_SIZE,
        "upload_delay_ms": 1.0,
        "timeout": 8.0,
        "port": "COM_TEST",
        "baud": 115200,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def firmware_config() -> str:
    return (Path(__file__).parents[2] / "src" / "config.h").read_text(
        encoding="utf-8"
    )


def firmware_integer_constant(name: str) -> int:
    match = re.search(
        rf"constexpr uint(?:8|16|32)_t {re.escape(name)} = ([0-9]+);",
        firmware_config(),
    )
    if not match:
        raise AssertionError(f"missing firmware integer constant {name}")
    return int(match.group(1))


class FakeSerialReaderClient:
    def __init__(self, upload_reply: list[str]) -> None:
        self.upload_reply = upload_reply
        self.uploaded: bytes | None = None
        self.command: str | None = None
        self.chunk_size: int | None = None
        self.closed = False

    def upload(self, payload: bytes, chunk_size: int) -> list[str]:
        self.command = f"UPLOAD {len(payload)}"
        self.uploaded = bytes(payload)
        self.chunk_size = chunk_size
        return self.upload_reply

    def close(self) -> None:
        self.closed = True


class FakeLineSerial:
    def __init__(self, lines: list[bytes]) -> None:
        self.lines = list(lines)
        self.writes: list[bytes] = []
        self.flushed = 0

    def readline(self) -> bytes:
        if self.lines:
            return self.lines.pop(0)
        return b""

    def write(self, data: bytes) -> int:
        self.writes.append(bytes(data))
        return len(data)

    def flush(self) -> None:
        self.flushed += 1


def fake_serial_client(lines: list[bytes]) -> tuple[SerialReaderClient, FakeLineSerial]:
    client = object.__new__(SerialReaderClient)
    serial = FakeLineSerial(lines)
    client.serial = serial
    client.timeout = 0.1
    client.upload_delay = 0.0
    return client, serial


class RsvpBookFormatTests(unittest.TestCase):
    def test_build_book_bytes_validates_with_cli_preflight(self) -> None:
        data, words = build_book_bytes(
            "One two three.",
            title="Tiny Test",
            author="Codex",
            wpm=275,
        )

        title, author, parsed_words = validate_rsvp_book(data)

        self.assertEqual(title, "Tiny Test")
        self.assertEqual(author, "Codex")
        self.assertEqual(parsed_words, words)

    def test_book_id_is_distinct_from_payload_crc(self) -> None:
        data, _words = build_book_bytes(
            "One two three.",
            title="Book Id",
            book_id=0x12345678,
        )
        fields = unpack_header(data)

        self.assertEqual(fields[3], 0x12345678)
        self.assertNotEqual(fields[3], fields[10])
        validate_rsvp_book(data)

    def test_same_text_imports_get_fresh_book_ids(self) -> None:
        first, _words = build_book_bytes("Repeatable text.", title="One")
        second, _words = build_book_bytes("Repeatable text.", title="Two")
        first_fields = unpack_header(first)
        second_fields = unpack_header(second)

        self.assertNotEqual(first_fields[3], second_fields[3])
        self.assertEqual(first_fields[10], second_fields[10])

    def test_crc_mismatch_is_rejected(self) -> None:
        data, _words = build_book_bytes("One two three.", title="Bad CRC")
        corrupted = bytearray(data)
        corrupted[-1] ^= 0x01

        with self.assertRaisesRegex(ValueError, "CRC"):
            validate_rsvp_book(bytes(corrupted))

    def test_word_count_mismatch_is_rejected(self) -> None:
        data, _words = build_book_bytes("One two three.", title="Bad Count")
        fields = list(unpack_header(data))
        fields[4] += 1
        corrupted = struct.pack(BOOK_HEADER_FORMAT, *fields) + data[BOOK_HEADER_SIZE:]

        with self.assertRaisesRegex(ValueError, "header says"):
            validate_rsvp_book(corrupted)

    def test_txt_upload_payload_uses_book_builder(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.txt"
            path.write_text("Alpha beta gamma.", encoding="utf-8")

            upload = build_upload_payload(
                upload_args(path, title="Sample", author="Tester", wpm=300)
            )
            title, author, words = validate_rsvp_book(upload.data)

            self.assertEqual(title, "Sample")
            self.assertEqual(author, "Tester")
            self.assertEqual(words, 3)
            self.assertEqual(upload.word_count, 3)
            self.assertEqual(upload.label, "Sample (3 words)")

    def test_describe_upload_payload_reports_local_preflight_details(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.txt"
            path.write_text("Alpha beta gamma.", encoding="utf-8")
            upload = build_upload_payload(
                upload_args(path, title="Sample", author="Tester", wpm=300)
            )

            description = describe_upload_payload(upload)

            self.assertIn("title: Sample", description)
            self.assertIn("author: Tester", description)
            self.assertIn("words: 3", description)
            self.assertIn(f"bytes: {len(upload.data)}", description)

    def test_inspect_command_prints_preflight_details(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.txt"
            path.write_text("Alpha beta gamma.", encoding="utf-8")

            with patch("sys.stdout", new_callable=io.StringIO) as stdout:
                result = inspect_command(
                    upload_args(path, title="Sample", author="Tester", wpm=300)
                )

            self.assertEqual(result, 0)
            self.assertIn("title: Sample", stdout.getvalue())
            self.assertIn("words: 3", stdout.getvalue())

    def test_convert_command_writes_valid_rsvp_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            input_path = Path(tmp) / "sample.txt"
            output_path = Path(tmp) / "out" / "sample.rsvp"
            input_path.write_text("Alpha beta gamma.", encoding="utf-8")

            args = upload_args(
                input_path,
                output=str(output_path),
                title="Sample",
                author="Tester",
                wpm=300,
            )

            with patch("sys.stdout", new_callable=io.StringIO) as stdout:
                result = convert_command(args)

            title, author, words = validate_rsvp_book(output_path.read_bytes())
            self.assertEqual(result, 0)
            self.assertEqual(title, "Sample")
            self.assertEqual(author, "Tester")
            self.assertEqual(words, 3)
            self.assertIn(f"created: {output_path}", stdout.getvalue())

    def test_convert_command_copies_preflighted_rsvp_input(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            input_path = Path(tmp) / "input.rsvp"
            output_path = Path(tmp) / "copy.rsvp"
            data, _words = build_book_bytes("Alpha beta.", title="Existing")
            input_path.write_bytes(data)

            args = upload_args(input_path, output=str(output_path))

            with patch("sys.stdout", new_callable=io.StringIO):
                result = convert_command(args)

            self.assertEqual(result, 0)
            self.assertEqual(output_path.read_bytes(), data)

    def test_write_file_atomic_replaces_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_path = Path(tmp) / "book.rsvp"
            output_path.write_bytes(b"old")

            write_file_atomic(output_path, b"new")

            self.assertEqual(output_path.read_bytes(), b"new")
            self.assertFalse((Path(tmp) / ".book.rsvp.tmp").exists())

    def test_write_file_atomic_cleans_temp_after_replace_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_path = Path(tmp) / "book.rsvp"
            output_path.write_bytes(b"old")
            temporary = Path(tmp) / ".book.rsvp.tmp"

            with patch.object(Path, "replace", side_effect=OSError("replace failed")):
                with self.assertRaises(OSError):
                    write_file_atomic(output_path, b"new")

            self.assertEqual(output_path.read_bytes(), b"old")
            self.assertFalse(temporary.exists())

    def test_epub_upload_payload_uses_spine_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.epub"
            write_sample_epub(path)

            upload = build_upload_payload(upload_args(path, wpm=300))
            title, author, words = validate_rsvp_book(upload.data)

            self.assertEqual(title, "EPUB Sample")
            self.assertEqual(author, "Writer")
            self.assertEqual(words, 6)
            self.assertEqual(upload.word_count, 6)
            self.assertEqual(upload.label, "EPUB Sample (6 words)")

    def test_epub_drm_marker_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "locked.epub"
            write_sample_epub(path, encrypted=True)

            with self.assertRaisesRegex(ValueError, "encrypted|DRM"):
                read_epub(str(path))

    def test_rsvp_upload_payload_is_preflighted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.rsvp"
            data, _words = build_book_bytes("Alpha beta.", title="Existing")
            path.write_bytes(data)

            upload = build_upload_payload(upload_args(path))

            self.assertEqual(upload.data, data)
            self.assertEqual(upload.word_count, 2)
            self.assertEqual(upload.label, "Existing (2 words)")

    def test_parse_state_ignores_non_integer_fields(self) -> None:
        self.assertEqual(
            parse_state("playing=0;wpm=250;word=1;total=33;note=ready"),
            {"playing": 0, "wpm": 250, "word": 1, "total": 33},
        )

    def test_parse_state_accepts_transport_prefix(self) -> None:
        self.assertEqual(
            parse_state("STATE playing=1;wpm=300;word=9;total=33"),
            {"playing": 1, "wpm": 300, "word": 9, "total": 33},
        )

    def test_parse_state_accepts_compact_status_prefix(self) -> None:
        self.assertEqual(
            parse_state("STATUS title=Book;playing=1;wpm=300;word=9;total=33"),
            {"playing": 1, "wpm": 300, "word": 9, "total": 33},
        )

    def test_extract_state_text_accepts_multiline_or_compact_status(self) -> None:
        self.assertEqual(
            extract_state_text("META v1|Book\nSTATE playing=0;wpm=250;word=1;total=3\nUPLOAD idle"),
            "STATE playing=0;wpm=250;word=1;total=3",
        )
        self.assertEqual(
            extract_state_text("STATUS title=Book;playing=0;wpm=250;word=1;total=3"),
            "STATUS title=Book;playing=0;wpm=250;word=1;total=3",
        )

    def test_default_upload_chunk_fits_serial_policy(self) -> None:
        self.assertGreaterEqual(DEFAULT_UPLOAD_CHUNK_SIZE, MIN_UPLOAD_CHUNK_SIZE)
        self.assertLessEqual(DEFAULT_UPLOAD_CHUNK_SIZE, MAX_UPLOAD_CHUNK_SIZE)
        self.assertLessEqual(MAX_UPLOAD_CHUNK_SIZE + 1, 244)

    def test_generated_words_fit_reader_word_limit(self) -> None:
        self.assertLessEqual(MAX_GENERATED_WORD_BYTES, MAX_READER_WORD_BYTES)

        words = normalise_words("supercalifragilisticexpialidocious café")
        self.assertTrue(words)
        self.assertTrue(
            all(len(word) <= MAX_GENERATED_WORD_BYTES for word in words)
        )

    def test_generated_words_preserve_utf8_latin_text(self) -> None:
        data, word_count = build_book_bytes(
            "Café déjà vu. Eén naïef meisje.",
            title="Accents",
        )

        words = payload_words(data)

        self.assertEqual(word_count, 6)
        self.assertEqual(words, ["Café", "déjà", "vu.", "Eén", "naïef", "meisje."])

    def test_utf8_hardware_sample_builds_without_losing_accents(self) -> None:
        sample = Path(__file__).parents[2] / "examples" / "utf8-dutch.txt"
        data, word_count = build_book_bytes(
            sample.read_text(encoding="utf-8"),
            title="UTF8 Dutch",
        )

        words = payload_words(data)

        self.assertEqual(word_count, 14)
        self.assertIn("Café", words)
        self.assertIn("Eén", words)
        self.assertIn("naïef", words)
        self.assertIn("Smörgåsbord", words)
        self.assertIn("cliëntèle", words)

    def test_tools_package_includes_import_modules(self) -> None:
        pyproject = Path(__file__).parents[1] / "pyproject.toml"
        modules = tomllib.loads(pyproject.read_text(encoding="utf-8"))["tool"][
            "setuptools"
        ]["py-modules"]

        self.assertIn("rsvp_cli", modules)
        self.assertIn("make_book", modules)
        self.assertIn("epub_import", modules)

    def test_book_format_doc_tracks_v1_constants(self) -> None:
        spec = (Path(__file__).parents[2] / "docs" / "book-format.md").read_text(
            encoding="utf-8"
        )

        self.assertIn("108-byte header", spec)
        self.assertIn("<4sHHIIIHH48s32sI", spec)
        self.assertIn("`50..800`", spec)
        self.assertIn("`1..23`", spec)
        self.assertIn("Random nonzero uint32", spec)

    def test_book_format_doc_test_vector_matches_builder(self) -> None:
        spec = (Path(__file__).parents[2] / "docs" / "book-format.md").read_text(
            encoding="utf-8"
        )
        data, word_count = build_book_bytes(
            "Café déjà vu.",
            title="Vector",
            author="Tester",
            wpm=250,
            book_id=0x12345678,
        )
        fields = unpack_header(data)
        payload = data[BOOK_HEADER_SIZE:]

        self.assertIn(f"wordCount: {word_count}", spec)
        self.assertIn(f"fileBytes: {len(data)}", spec)
        self.assertIn(f"payloadBytes: {fields[5]}", spec)
        self.assertIn(f"payloadCrc32: 0x{fields[10]:08x}", spec)
        self.assertIn(f"sha256: {hashlib.sha256(data).hexdigest()}", spec)
        self.assertIn(f"payloadHex: {payload.hex()}", spec)

    def test_serial_protocol_doc_tracks_v1_policy(self) -> None:
        spec = (Path(__file__).parents[2] / "docs" / "serial-protocol.md").read_text(
            encoding="utf-8"
        )

        self.assertIn("RSVP/1", spec)
        self.assertIn("UPLOAD <byteCount>", spec)
        self.assertIn("STATUS", spec)
        self.assertIn("WPM:+25", spec)
        self.assertIn("physical cable access is the trust boundary", spec.lower())

    def test_wpm_control_accepts_absolute_and_relative_steps(self) -> None:
        self.assertEqual(wpm_control_command("300"), "WPM:300")
        self.assertEqual(wpm_control_command("+25"), "WPM:+25")
        self.assertEqual(wpm_control_command("-25"), "WPM:-25")

    def test_wpm_control_rejects_invalid_values(self) -> None:
        for value in ("49", "801", "+10", "-10", "fast"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    wpm_control_command(value)

    def test_serial_error_message_handles_timeout(self) -> None:
        message = serial_error_message(TimeoutError())

        self.assertIsNotNone(message)
        self.assertIn("Timed out", message or "")
        self.assertIn("serial", message or "")

    def test_serial_error_message_ignores_programming_errors(self) -> None:
        self.assertIsNone(serial_error_message(ValueError("bad input")))

    def test_serial_frame_reader_ignores_boot_noise(self) -> None:
        client, serial = fake_serial_client(
            [
                b"=== RSVP reader boot ===\n",
                b"LittleFS mounted\n",
                b"RSVP/1\n",
                b"STATE playing=0;wpm=250;word=1;total=3\n",
                b"UPLOAD idle\n",
                b".\n",
            ]
        )

        self.assertEqual(
            client.read_frame(),
            ["STATE playing=0;wpm=250;word=1;total=3", "UPLOAD idle"],
        )
        self.assertEqual(serial.writes, [])

    def test_serial_upload_streams_raw_bytes_after_ready(self) -> None:
        client, serial = fake_serial_client(
            [
                b"RSVP/1\n",
                b"UPLOAD READY\n",
                b".\n",
                b"RSVP/1\n",
                b"STATE playing=0;wpm=250;word=1;total=2\n",
                b"UPLOAD OK:activated\n",
                b".\n",
            ]
        )

        with patch("sys.stdout", new_callable=io.StringIO):
            lines = client.upload(b"abcdef", 4)

        self.assertEqual(
            serial.writes,
            [b"UPLOAD 6\n", b"abcd", b"ef"],
        )
        self.assertEqual(lines, ["STATE playing=0;wpm=250;word=1;total=2", "UPLOAD OK:activated"])

    def test_cli_reader_limits_match_firmware(self) -> None:
        self.assertEqual(MIN_WPM, firmware_integer_constant("MIN_WPM"))
        self.assertEqual(MAX_WPM, firmware_integer_constant("MAX_WPM"))
        self.assertEqual(
            MAX_READER_WORD_BYTES + 1,
            firmware_integer_constant("WORD_BUFFER_SIZE"),
        )


class RsvpUploadCommandTests(unittest.TestCase):
    def test_upload_command_sends_raw_serial_upload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.txt"
            path.write_text("One two three.", encoding="utf-8")
            client = FakeSerialReaderClient(
                [
                    "UPLOAD OK:activated",
                    "STATE playing=0;wpm=250;word=1;total=3",
                    "UPLOAD OK:activated",
                ]
            )

            def fake_connect_reader(_args):
                return client

            with patch.object(rsvp_cli, "connect_reader", fake_connect_reader):
                with patch("sys.stdout", new_callable=io.StringIO):
                    result = upload_command(upload_args(path, chunk_size=32))

            self.assertEqual(result, 0)
            self.assertTrue(client.closed)
            self.assertIsNotNone(client.uploaded)
            self.assertEqual(client.command, f"UPLOAD {len(client.uploaded or b'')}")
            title, _author, words = validate_rsvp_book(client.uploaded or b"")
            self.assertEqual(title, "Sample")
            self.assertEqual(words, 3)
            self.assertEqual(payload_words(client.uploaded or b""), ["One", "two", "three."])
            self.assertEqual(client.chunk_size, 32)

    def test_upload_command_rejects_error_frame(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.txt"
            path.write_text("One two three.", encoding="utf-8")
            client = FakeSerialReaderClient(["ERR validation_or_activation_failed"])

            def fake_connect_reader(_args):
                return client

            with patch.object(rsvp_cli, "connect_reader", fake_connect_reader):
                with patch("sys.stdout", new_callable=io.StringIO):
                    with self.assertRaisesRegex(SystemExit, "ERR validation"):
                        upload_command(upload_args(path, chunk_size=32))

            self.assertTrue(client.closed)


def write_sample_epub(path: Path, *, encrypted: bool = False) -> None:
    with ZipFile(path, "w") as epub:
        epub.writestr("mimetype", "application/epub+zip")
        epub.writestr(
            "META-INF/container.xml",
            """<?xml version="1.0"?>
            <container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
              <rootfiles>
                <rootfile full-path="OEBPS/content.opf"
                  media-type="application/oebps-package+xml"/>
              </rootfiles>
            </container>""",
        )
        if encrypted:
            epub.writestr("META-INF/encryption.xml", "<encryption/>")
        epub.writestr(
            "OEBPS/content.opf",
            """<?xml version="1.0"?>
            <package xmlns="http://www.idpf.org/2007/opf"
              xmlns:dc="http://purl.org/dc/elements/1.1/">
              <metadata>
                <dc:title>EPUB Sample</dc:title>
                <dc:creator>Writer</dc:creator>
              </metadata>
              <manifest>
                <item id="c1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
                <item id="c2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
              </manifest>
              <spine>
                <itemref idref="c1"/>
                <itemref idref="c2"/>
              </spine>
            </package>""",
        )
        epub.writestr(
            "OEBPS/chapter1.xhtml",
            "<html><body><h1>One</h1><p>Two three.</p></body></html>",
        )
        epub.writestr(
            "OEBPS/chapter2.xhtml",
            "<html><body><p>Four five six.</p></body></html>",
        )


if __name__ == "__main__":
    unittest.main()
