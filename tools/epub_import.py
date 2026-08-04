from __future__ import annotations

from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import PurePosixPath
from zipfile import BadZipFile, ZipFile
import posixpath
import xml.etree.ElementTree as ET


CONTAINER_PATH = "META-INF/container.xml"
ENCRYPTION_PATH = "META-INF/encryption.xml"
CONTAINER_NS = {"c": "urn:oasis:names:tc:opendocument:xmlns:container"}
OPF_NS = {
    "opf": "http://www.idpf.org/2007/opf",
    "dc": "http://purl.org/dc/elements/1.1/",
}


@dataclass(frozen=True)
class EpubDocument:
    title: str
    author: str
    text: str
    chapters: list[str]


class HtmlTextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self._chunks: list[str] = []
        self._skip_depth = 0

    def handle_starttag(self, tag: str, attrs) -> None:
        if tag in {"script", "style"}:
            self._skip_depth += 1
        elif tag in {"p", "div", "section", "article", "br", "h1", "h2", "h3"}:
            self._chunks.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag in {"script", "style"} and self._skip_depth:
            self._skip_depth -= 1
        elif tag in {"p", "div", "section", "article", "h1", "h2", "h3"}:
            self._chunks.append("\n")

    def handle_data(self, data: str) -> None:
        if not self._skip_depth:
            self._chunks.append(data)

    def text(self) -> str:
        lines = (" ".join(line.split()) for line in "".join(self._chunks).splitlines())
        return "\n\n".join(line for line in lines if line)


def _read_xml(epub: ZipFile, path: str) -> ET.Element:
    try:
        return ET.fromstring(epub.read(path))
    except KeyError as exc:
        raise ValueError(f"EPUB is missing {path}") from exc
    except ET.ParseError as exc:
        raise ValueError(f"EPUB has invalid XML in {path}") from exc


def _rootfile_path(epub: ZipFile) -> str:
    container = _read_xml(epub, CONTAINER_PATH)
    rootfile = container.find(".//c:rootfile", CONTAINER_NS)
    if rootfile is None:
        raise ValueError("EPUB container does not name a package document")

    path = rootfile.attrib.get("full-path", "").strip()
    if not path:
        raise ValueError("EPUB package path is empty")
    return path


def _metadata_text(package: ET.Element, name: str, fallback: str) -> str:
    node = package.find(f".//dc:{name}", OPF_NS)
    if node is None or not node.text or not node.text.strip():
        return fallback
    return " ".join(node.text.split())


def _spine_documents(package: ET.Element, package_path: str) -> list[str]:
    manifest = package.find("opf:manifest", OPF_NS)
    spine = package.find("opf:spine", OPF_NS)
    if manifest is None or spine is None:
        raise ValueError("EPUB package is missing manifest or spine")

    manifest_items = {
        item.attrib.get("id"): item.attrib.get("href", "")
        for item in manifest.findall("opf:item", OPF_NS)
    }

    base = PurePosixPath(package_path).parent
    documents: list[str] = []
    for itemref in spine.findall("opf:itemref", OPF_NS):
        href = manifest_items.get(itemref.attrib.get("idref"))
        if not href:
            continue

        documents.append(posixpath.normpath(str(base / href)))

    if not documents:
        raise ValueError("EPUB spine does not contain readable documents")
    return documents


def _decode_html(raw: bytes) -> str:
    for encoding in ("utf-8-sig", "utf-8", "windows-1252"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def _html_to_text(raw: bytes) -> str:
    parser = HtmlTextExtractor()
    parser.feed(_decode_html(raw))
    parser.close()
    return parser.text()


def read_epub(path: str) -> EpubDocument:
    try:
        with ZipFile(path) as epub:
            names = set(epub.namelist())
            if ENCRYPTION_PATH in names:
                raise ValueError("EPUB appears to be encrypted or DRM-protected")

            package_path = _rootfile_path(epub)
            package = _read_xml(epub, package_path)
            title = _metadata_text(package, "title", "Untitled")
            author = _metadata_text(package, "creator", "Unknown")

            chapters: list[str] = []
            for document_path in _spine_documents(package, package_path):
                try:
                    text = _html_to_text(epub.read(document_path))
                except KeyError as exc:
                    raise ValueError(f"EPUB spine references missing file {document_path}") from exc

                if text:
                    chapters.append(text)

            if not chapters:
                raise ValueError("EPUB does not contain readable text")

            return EpubDocument(
                title=title,
                author=author,
                text="\n\n".join(chapters),
                chapters=chapters,
            )
    except BadZipFile as exc:
        raise ValueError("EPUB is not a valid ZIP archive") from exc
