#!/usr/bin/env python3
"""Generates the shelf-index host-test fixtures in test/fixtures/shelf/.

Only small, deterministic files are produced so the fixtures stay reviewable:
  book.xtc / broken.xtc        XTC header + one page (title/author at 0x38/0xB8)
  fb2_cp1251.fb2               FB2 in Windows-1251 (the encoding path FB2 supports)
  author - title.txt           OPDS-style filename metadata
  just-title.md                Markdown without an author in the name
  broken.epub                  garbage with an .epub name
  broken.fb2.zip               valid zip with no *.fb2 entry
  epub_inside.zip              EPUB content behind a plain .zip name
  fb2_inside.zip               FB2 content behind a plain .zip name

Run from the repository root:  python3 test/fixtures/shelf/generate_shelf_fixtures.py
"""

import pathlib
import struct
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
OUT = ROOT / "test" / "fixtures" / "shelf"
FIXTURES = ROOT / "test" / "fixtures"


def xtc_file(title: str, author: str, pages: int = 1) -> bytes:
    header = struct.pack(
        "<IBBHBBBBIQQQQII",
        0x00435458,  # magic "XTC\0"
        1,  # versionMajor
        0,  # versionMinor
        pages,
        0,  # readDirection
        1,  # hasMetadata
        0,  # hasThumbnails
        0,  # hasChapters
        1,  # currentPage
        0,  # metadataOffset
        0x100,  # pageTableOffset
        0x140,  # dataOffset
        0,  # thumbOffset
        0,  # chapterOffset
        0,  # padding
    )
    assert len(header) == 56, len(header)

    title_slot = title.encode("utf-8")[:127].ljust(128, b"\x00")
    author_slot = author.encode("utf-8")[:63].ljust(64, b"\x00")
    blob = bytearray(header + title_slot + author_slot)
    assert len(blob) == 0xB8 + 64

    blob.extend(b"\x00" * (0x100 - len(blob)))
    page_width, page_height = 8, 8
    bitmap = bytes([0xAA] * ((page_width + 7) // 8 * page_height))
    page_data = struct.pack("<IHHBBIQ", 0x00475458, page_width, page_height, 0, 0, len(bitmap), 0) + bitmap
    assert len(page_data) == 22 + len(bitmap)
    blob.extend(struct.pack("<QIHH", 0x140, len(page_data), page_width, page_height))
    blob.extend(b"\x00" * (0x140 - len(blob)))
    blob.extend(page_data)
    return bytes(blob)


def fb2_cp1251() -> bytes:
    xml = (
        '<?xml version="1.0" encoding="windows-1251"?>\n'
        '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
        "<description><title-info>"
        "<genre>prose</genre>"
        "<author><first-name>Иван</first-name><last-name>Тургенев</last-name></author>"
        "<book-title>Отцы и дети</book-title>"
        "<lang>ru</lang>"
        "</title-info></description>"
        "<body><section><title><p>Глава 1</p></title><p>Текст главы.</p></section></body>"
        "</FictionBook>\n"
    )
    return xml.encode("windows-1251")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    (OUT / "book.xtc").write_bytes(xtc_file("Тестовая книга", "Александр Тестов"))
    (OUT / "broken.xtc").write_bytes(b"XTC\x00" + b"\x00" * 20)
    (OUT / "fb2_cp1251.fb2").write_bytes(fb2_cp1251())
    (OUT / "author - title.txt").write_text("Текст книги.\n", encoding="utf-8")
    (OUT / "just-title.md").write_text("# Заголовок\n", encoding="utf-8")
    (OUT / "broken.epub").write_bytes(b"not a zip at all")

    with zipfile.ZipFile(OUT / "broken.fb2.zip", "w") as archive:
        archive.writestr("readme.txt", "no fb2 entry here")
    with zipfile.ZipFile(OUT / "epub_inside.zip", "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml", (FIXTURES / "moby-dick" / "container.xml").read_text())
        archive.writestr("OEBPS/content.opf", (FIXTURES / "moby-dick" / "content.opf").read_text())
    (OUT / "fb2_inside.zip").write_bytes((FIXTURES / "inline_footnote_full.fb2.zip").read_bytes())

    for path in sorted(OUT.iterdir()):
        print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
