from __future__ import annotations

import argparse
import os
import struct
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

import serial
from serial.tools import list_ports

from epub_import import read_epub
from make_book import (
    HEADER_FORMAT as BOOK_HEADER_FORMAT,
    HEADER_SIZE as BOOK_HEADER_SIZE,
    MAX_WPM,
    MIN_WPM,
    VERSION as BOOK_VERSION,
    build_book_bytes,
)


READER_NAME = "RSVP Reader"
FRAME_START = "RSVP/1"
FRAME_END = "."
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 60.0
MAX_READER_WORD_BYTES = 23
MIN_UPLOAD_CHUNK_SIZE = 20
MAX_UPLOAD_CHUNK_SIZE = 240
DEFAULT_UPLOAD_CHUNK_SIZE = 96
DEFAULT_UPLOAD_DELAY_MS = 1.0
UPLOAD_PROGRESS_STEP_BYTES = 4096


@dataclass(frozen=True)
class UploadPayload:
    data: bytes
    label: str
    word_count: int


@dataclass(frozen=True)
class SerialPortInfo:
    device: str
    description: str
    hwid: str


def decode_c_string(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def parse_state(state: str) -> dict[str, int]:
    parsed: dict[str, int] = {}
    if state.startswith("STATE "):
        state = state[6:]
    elif state.startswith("STATUS "):
        state = state[7:]

    for part in state.split(";"):
        key, sep, value = part.partition("=")
        if not sep:
            continue

        try:
            parsed[key] = int(value)
        except ValueError:
            continue

    return parsed


def extract_state_text(status: str) -> str:
    return next(
        (line for line in status.splitlines() if line.startswith("STATE ")),
        status,
    )


def wpm_control_command(value: str) -> str:
    if value in {"+25", "-25"}:
        return f"WPM:{value}"

    if value.startswith(("+", "-")):
        raise ValueError("relative WPM changes must be +25 or -25")

    try:
        requested = int(value)
    except ValueError as exc:
        raise ValueError("WPM must be a number, +25, or -25") from exc

    if not MIN_WPM <= requested <= MAX_WPM:
        raise ValueError(f"WPM must be between {MIN_WPM} and {MAX_WPM}")

    return f"WPM:{requested}"


def validate_rsvp_book(data: bytes) -> tuple[str, str, int]:
    if len(data) < BOOK_HEADER_SIZE:
        raise ValueError("book is smaller than the RSVP header")

    (
        magic,
        version,
        header_size,
        _book_id,
        word_count,
        payload_bytes,
        default_wpm,
        chapter_count,
        raw_title,
        raw_author,
        payload_crc32,
    ) = struct.unpack(BOOK_HEADER_FORMAT, data[:BOOK_HEADER_SIZE])

    if magic != b"RSVP":
        raise ValueError("bad magic")
    if version not in (1, 2):
        raise ValueError(f"unsupported book version: {version}")
    if header_size != BOOK_HEADER_SIZE:
        raise ValueError(f"unexpected header size: {header_size}")
    if word_count <= 0:
        raise ValueError("book has no words")
    if not MIN_WPM <= default_wpm <= MAX_WPM:
        raise ValueError(f"default WPM out of range: {default_wpm}")

    chapter_table_bytes = (chapter_count * 4) if (version == 2 and chapter_count > 0) else 0
    if len(data) != BOOK_HEADER_SIZE + chapter_table_bytes + payload_bytes:
        raise ValueError("file size does not match header payload length")

    payload = data[BOOK_HEADER_SIZE + chapter_table_bytes:]
    offset = 0
    words = 0

    while offset < len(payload):
        length = payload[offset]
        offset += 1

        if length <= 0 or length > MAX_READER_WORD_BYTES:
            raise ValueError(f"invalid word length at payload offset {offset - 1}")

        if offset + length > len(payload):
            raise ValueError("word record extends past payload")

        offset += length
        words += 1

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    if words != word_count:
        raise ValueError(f"header says {word_count} words but payload has {words}")
    if crc != payload_crc32:
        raise ValueError("payload CRC mismatch")
    return decode_c_string(raw_title), decode_c_string(raw_author), word_count


def describe_upload_payload(upload: UploadPayload) -> str:
    title, author, words = validate_rsvp_book(upload.data)
    return "\n".join(
        [
            f"title: {title}",
            f"author: {author}",
            f"words: {words}",
            f"bytes: {len(upload.data)}",
        ]
    )


def write_file_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")

    try:
        temporary.write_bytes(data)
        temporary.replace(path)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def build_upload_payload(args: argparse.Namespace) -> UploadPayload:
    path = Path(args.path)

    if not path.exists():
        raise SystemExit(f"File not found: {path}")

    if path.suffix.lower() == ".rsvp":
        data = path.read_bytes()
        try:
            title, _author, word_count = validate_rsvp_book(data)
        except ValueError as exc:
            raise SystemExit(f"Invalid .rsvp book: {exc}") from exc

        label = f"{title or path.name} ({word_count} words)"
        return UploadPayload(data=data, label=label, word_count=word_count)

    if path.suffix.lower() == ".epub":
        try:
            epub = read_epub(str(path))
        except ValueError as exc:
            raise SystemExit(f"Could not import EPUB: {exc}") from exc

        title = args.title or epub.title or path.stem.replace("_", " ").title()
        author = args.author if args.author != "Unknown" else epub.author
        payload, word_count = build_book_bytes(
            epub.chapters,
            title=title,
            author=author,
            wpm=args.wpm,
        )
        return UploadPayload(
            data=payload,
            label=f"{title} ({word_count} words)",
            word_count=word_count,
        )

    if path.suffix.lower() != ".txt":
        raise SystemExit("Only .epub, .txt, and .rsvp uploads are supported.")

    title = args.title or path.stem.replace("_", " ").title()
    payload, word_count = build_book_bytes(
        path.read_text(encoding="utf-8"),
        title=title,
        author=args.author,
        wpm=args.wpm,
    )
    return UploadPayload(
        data=payload,
        label=f"{title} ({word_count} words)",
        word_count=word_count,
    )


def available_ports() -> list[SerialPortInfo]:
    return [
        SerialPortInfo(
            device=port.device,
            description=port.description or "",
            hwid=port.hwid or "",
        )
        for port in list_ports.comports()
    ]


def likely_reader_ports() -> list[SerialPortInfo]:
    candidates: list[SerialPortInfo] = []
    for port in available_ports():
        haystack = f"{port.device} {port.description} {port.hwid}".lower()
        if any(token in haystack for token in ("usb", "cdc", "uart", "esp32", "jtag")):
            candidates.append(port)
    return candidates


def choose_port(explicit_port: str | None = None) -> str:
    if explicit_port:
        return explicit_port

    env_port = os.environ.get("RSVP_PORT")
    if env_port:
        return env_port

    candidates = likely_reader_ports()
    if len(candidates) == 1:
        return candidates[0].device

    if not candidates:
        raise SystemExit("No serial reader port found. Use `rsvp ports` or pass --port COMx.")

    devices = ", ".join(port.device for port in candidates)
    raise SystemExit(f"Multiple serial ports look possible ({devices}). Pass --port COMx.")


class SerialReaderClient:
    def __init__(
        self,
        port: str,
        *,
        baud: int = DEFAULT_BAUD,
        timeout: float = DEFAULT_TIMEOUT,
        upload_delay_ms: float = DEFAULT_UPLOAD_DELAY_MS,
    ) -> None:
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baud
        self.serial.timeout = 0.1
        self.serial.write_timeout = timeout
        self.serial.rts = False
        self.serial.dtr = False
        self.serial.open()
        self.serial.reset_input_buffer()
        self.timeout = timeout
        self.upload_delay = max(0.0, upload_delay_ms / 1000.0)

    def close(self) -> None:
        self.serial.close()

    def request(self, command: str) -> list[str]:
        self.serial.write(command.encode("utf-8") + b"\n")
        self.serial.flush()
        return self.read_frame()

    def upload(self, payload: bytes, chunk_size: int) -> list[str]:
        ready = self.request(f"UPLOAD {len(payload)} {chunk_size}")
        if not any(line == "UPLOAD READY" for line in ready):
            return ready

        sent = 0
        next_progress = 0
        while sent < len(payload):
            chunk = payload[sent : sent + chunk_size]
            self.serial.write(chunk)
            self.serial.flush()
            sent += len(chunk)
            if self.upload_delay:
                time.sleep(self.upload_delay)
            if sent >= next_progress or sent == len(payload):
                print(f"\ruploaded {sent}/{len(payload)} bytes", end="", flush=True)
                next_progress = sent + UPLOAD_PROGRESS_STEP_BYTES

            lines = self.read_frame()
            if any(line.startswith("ERR ") for line in lines):
                print()
                return lines

            if sent < len(payload) and not any(
                line.startswith("UPLOAD CONT ") for line in lines
            ):
                return lines

            if sent == len(payload):
                return lines

        raise TimeoutError("Upload ended without an RSVP response frame")

    def read_frame(self) -> list[str]:
        deadline = time.monotonic() + self.timeout
        in_frame = False
        lines: list[str] = []

        while time.monotonic() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if not in_frame:
                if line == FRAME_START:
                    in_frame = True
                    lines = []
                continue

            if line == FRAME_END:
                return lines

            lines.append(line)

        raise TimeoutError("Timed out waiting for RSVP serial response frame")


def serial_error_message(exc: BaseException) -> str | None:
    if isinstance(exc, TimeoutError):
        return (
            "Timed out while talking to the RSVP reader over serial.\n"
            "Check the port, close serial monitors, and make sure the board is powered."
        )

    if isinstance(exc, serial.SerialException):
        detail = str(exc).strip()
        message = "Serial error while talking to the RSVP reader."
        if detail:
            message = f"{message}\n{detail}"
        return f"{message}\nRun `rsvp ports` and pass --port COMx if needed."

    return None


def require_ok_frame(lines: list[str]) -> None:
    error = next((line for line in lines if line.startswith("ERR ")), None)
    if error:
        raise SystemExit(error)


def frame_text(lines: list[str]) -> str:
    return "\n".join(lines)


def ports_command(_args: argparse.Namespace) -> int:
    ports = available_ports()
    if not ports:
        print("No serial ports found.")
        return 1

    for port in ports:
        print(f"{port.device}  {port.description}  {port.hwid}".rstrip())
    return 0


def connect_reader(args: argparse.Namespace) -> SerialReaderClient:
    return SerialReaderClient(
        choose_port(args.port),
        baud=args.baud,
        timeout=args.timeout,
        upload_delay_ms=getattr(args, "upload_delay_ms", DEFAULT_UPLOAD_DELAY_MS),
    )


def status_command(args: argparse.Namespace) -> int:
    client = connect_reader(args)
    try:
        lines = client.request("STATUS")
    finally:
        client.close()

    require_ok_frame(lines)
    print(frame_text(lines))
    return 0


def write_control(command: str, args: argparse.Namespace) -> int:
    client = connect_reader(args)
    try:
        lines = client.request(command)
    finally:
        client.close()

    require_ok_frame(lines)
    print(frame_text(lines))
    return 0


def play_command(args: argparse.Namespace) -> int:
    return write_control("PLAY", args)


def pause_command(args: argparse.Namespace) -> int:
    return write_control("PAUSE", args)


def wpm_command(args: argparse.Namespace) -> int:
    try:
        command = wpm_control_command(args.value)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    return write_control(command, args)


def inspect_command(args: argparse.Namespace) -> int:
    upload = build_upload_payload(args)
    print(describe_upload_payload(upload))
    return 0


def convert_command(args: argparse.Namespace) -> int:
    upload = build_upload_payload(args)
    output = Path(args.output)
    write_file_atomic(output, upload.data)

    print(f"created: {output}")
    print(describe_upload_payload(upload))
    return 0


def upload_command(args: argparse.Namespace) -> int:
    upload = build_upload_payload(args)
    payload = upload.data
    chunk_size = args.chunk_size

    if not MIN_UPLOAD_CHUNK_SIZE <= chunk_size <= MAX_UPLOAD_CHUNK_SIZE:
        raise SystemExit(
            f"--chunk-size must be between {MIN_UPLOAD_CHUNK_SIZE} "
            f"and {MAX_UPLOAD_CHUNK_SIZE}."
        )

    client = connect_reader(args)
    try:
        lines = client.upload(payload, chunk_size)
        require_ok_frame(lines)
        state = extract_state_text(frame_text(lines))
        parsed_state = parse_state(state)
        if parsed_state.get("total") != upload.word_count:
            raise SystemExit(
                "Upload activated, but reader state reports "
                f"{parsed_state.get('total')} words instead of {upload.word_count}."
            )
    finally:
        client.close()

    print()
    print(f"uploaded: {upload.label}")
    print(frame_text(lines))
    return 0


def add_book_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("path")
    parser.add_argument("--title")
    parser.add_argument("--author", default="Unknown")
    parser.add_argument("--wpm", type=int, default=250)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="rsvp")
    parser.add_argument("--port", help="serial port, for example COM5")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)

    subparsers = parser.add_subparsers(dest="command", required=True)

    ports = subparsers.add_parser("ports", help="list serial ports")
    ports.set_defaults(func=ports_command)

    status = subparsers.add_parser("status", help="read metadata and state")
    status.set_defaults(func=status_command)

    play = subparsers.add_parser("play", help="start reading")
    play.set_defaults(func=play_command)

    pause = subparsers.add_parser("pause", help="pause reading")
    pause.set_defaults(func=pause_command)

    wpm = subparsers.add_parser("wpm", help="set reading speed, or use +25/-25")
    wpm.add_argument("value")
    wpm.set_defaults(func=wpm_command)

    inspect = subparsers.add_parser(
        "inspect",
        help="convert and validate a .epub, .txt, or .rsvp book without uploading",
    )
    add_book_args(inspect)
    inspect.set_defaults(func=inspect_command)

    convert = subparsers.add_parser(
        "convert",
        help="convert a .epub, .txt, or .rsvp book to a local .rsvp file",
    )
    add_book_args(convert)
    convert.add_argument("output")
    convert.set_defaults(func=convert_command)

    upload = subparsers.add_parser("upload", help="upload a .epub, .txt, or .rsvp book")
    add_book_args(upload)
    upload.add_argument("--timeout", type=float, default=argparse.SUPPRESS)
    upload.add_argument("--chunk-size", type=int, default=DEFAULT_UPLOAD_CHUNK_SIZE)
    upload.add_argument("--upload-delay-ms", type=float, default=DEFAULT_UPLOAD_DELAY_MS)
    upload.set_defaults(func=upload_command)

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    try:
        raise SystemExit(args.func(args))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except (serial.SerialException, TimeoutError) as exc:
        raise SystemExit(serial_error_message(exc)) from None


if __name__ == "__main__":
    main()
