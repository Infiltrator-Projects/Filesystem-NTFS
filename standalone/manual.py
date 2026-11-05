"""Compile the provider's illustrated manual from editable PDF page commands."""
from pathlib import Path


def build(destination, source, version):
    pages = Path(source).read_bytes().replace(b"@VERSION@", version.encode("ascii")).split(b"\n%%Page\n")
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b""]
    for font in (b"Helvetica", b"Helvetica-Bold", b"Courier"):
        objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /" + font + b" >>")
    kids = []
    for page in pages:
        kids.append(f"{len(objects) + 1} 0 R".encode("ascii"))
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                       b"/Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> "
                       + f"/Contents {len(objects) + 2} 0 R >>".encode("ascii"))
        content = page.rstrip(b"\n") + b"\n"
        objects.append(f"<< /Length {len(content)} >>\nstream\n".encode("ascii") + content + b"endstream")
    objects[1] = b"<< /Type /Pages /Kids [" + b" ".join(kids) + f"] /Count {len(kids)} >>".encode("ascii")
    pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for index, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf.extend(f"{index} 0 obj\n".encode("ascii") + obj + b"\nendobj\n")
    xref, size = len(pdf), len(objects) + 1
    pdf.extend(f"xref\n0 {size}\n0000000000 65535 f \n".encode("ascii"))
    for offset in offsets:
        pdf.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    pdf.extend(f"trailer\n<< /Size {size} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    Path(destination).write_bytes(pdf)
