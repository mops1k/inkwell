#!/usr/bin/env python3
"""Generates the shelf-index host-test fixtures in test/fixtures/shelf/.

Only small, deterministic files are produced so the fixtures stay reviewable:
  book.xtc / broken.xtc        XTC header + one page (title/author at 0x38/0xB8)
  fb2_cp1251.fb2               FB2 in Windows-1251 (the encoding path FB2 supports),
                               with <publish-info> year/publisher
  fb2_date_only.fb2            FB2 without <publish-info>: year comes from <date>
  publication.epub             minimal EPUB declaring dc:date/publisher/dcterms:extent
  author - title.txt           OPDS-style filename metadata
  just-title.md                Markdown without an author in the name
  broken.epub                  garbage with an .epub name
  broken.fb2.zip               valid zip with no *.fb2 entry
  epub_inside.zip              EPUB content behind a plain .zip name
  fb2_inside.zip               FB2 content behind a plain .zip name

Run from the repository root:  python3 test/fixtures/shelf/generate_shelf_fixtures.py
"""

import io
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
        "<date value='1862-01-01'>1862</date>"
        "<lang>ru</lang>"
        "</title-info>"
        "<publish-info>"
        "<publisher>Художественная литература</publisher>"
        "<year>1965</year>"
        "</publish-info></description>"
        "<body><section><title><p>Глава 1</p></title><p>Текст главы.</p></section></body>"
        "</FictionBook>\n"
    )
    return xml.encode("windows-1251")


def fb2_date_only() -> bytes:
    """FB2 without <publish-info>: the year must come from <title-info><date>."""
    xml = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
        "<description><title-info>"
        "<genre>prose</genre>"
        "<book-title>Дата без публикации</book-title>"
        "<date value='1984-06-08'>8 июня 1984</date>"
        "<lang>ru</lang>"
        "</title-info></description>"
        "<body><section><p>Текст.</p></section></body>"
        "</FictionBook>\n"
    )
    return xml.encode("utf-8")


def publication_epub() -> bytes:
    """Minimal EPUB whose OPF declares dc:date, dc:publisher and dcterms:extent.

    Exists to pin the shelf's publication metadata: the card must show the
    declared year/publisher/pages, and a produced size ("1.2 MB") in
    dcterms:extent must never be read as a page count.
    """
    container = (
        '<?xml version="1.0"?>'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        "<rootfiles><rootfile full-path='OEBPS/content.opf' media-type='application/oebps-package+xml'/>"
        "</rootfiles></container>"
    )
    opf = (
        '<?xml version="1.0" encoding="utf-8"?>'
        "<package xmlns='http://www.idpf.org/2007/opf' version='2.0' unique-identifier='BookId'>"
        "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>"
        "<dc:title>Публикационная запись</dc:title>"
        "<dc:creator>Тест Тестов</dc:creator>"
        "<dc:language>ru</dc:language>"
        "<dc:date>1984-06-08</dc:date>"
        "<dc:publisher>Тестовое издательство</dc:publisher>"
        "<dc:identifier id='BookId'>urn:uuid:shelf-publication-0001</dc:identifier>"
        "<meta property='dcterms:extent'>320 pages</meta>"
        "</metadata>"
        "<manifest>"
        "<item id='ch1' href='ch1.xhtml' media-type='application/xhtml+xml'/>"
        "</manifest>"
        "<spine><itemref idref='ch1'/></spine>"
        "</package>"
    )
    chapter = (
        '<?xml version="1.0" encoding="utf-8"?>'
        "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Глава</title></head>"
        "<body><p>Текст.</p></body></html>"
    )
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml", container)
        archive.writestr("OEBPS/content.opf", opf)
        archive.writestr("OEBPS/ch1.xhtml", chapter)
    return buffer.getvalue()


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    (OUT / "book.xtc").write_bytes(xtc_file("Тестовая книга", "Александр Тестов"))
    (OUT / "broken.xtc").write_bytes(b"XTC\x00" + b"\x00" * 20)
    (OUT / "fb2_cp1251.fb2").write_bytes(fb2_cp1251())
    (OUT / "fb2_date_only.fb2").write_bytes(fb2_date_only())
    (OUT / "publication.epub").write_bytes(publication_epub())
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
