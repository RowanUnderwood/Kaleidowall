"""Regenerate checked-in PNG/ICO assets from the geometric SVG (requires Pillow).

Normal application builds use the checked-in ICO and do not run this script.
Only the SVG's rect/polygon primitives are supported; fail on unsupported artwork.
"""

from pathlib import Path
import struct
import xml.etree.ElementTree as ET
from io import BytesIO

from PIL import Image, ImageDraw


ASSETS = Path(__file__).resolve().parents[1] / "assets"
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def render(size):
    scale = size * 4 / 256
    image = Image.new("RGBA", (size * 4, size * 4))
    draw = ImageDraw.Draw(image)
    root = ET.parse(ASSETS / "kaleidowall.svg").getroot()
    for element in root:
        tag = element.tag.rsplit("}", 1)[-1]
        if tag in ("title", "desc"):
            continue
        fill = element.attrib["fill"]
        if tag == "rect":
            x, y, width, height, radius = (
                float(element.attrib[key]) * scale
                for key in ("x", "y", "width", "height", "rx")
            )
            draw.rounded_rectangle((x, y, x + width, y + height), radius=radius, fill=fill)
        elif tag == "polygon":
            points = [
                tuple(float(value) * scale for value in pair.split(","))
                for pair in element.attrib["points"].split()
            ]
            draw.polygon(points, fill=fill)
        else:
            raise ValueError(f"Unsupported SVG element: {tag}")
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    render(512).save(ASSETS / "kaleidowall.png")
    # Render each resolution separately for clean small icons. PNG-backed ICO
    # entries preserve full alpha and are supported by Windows Vista and later.
    payloads = []
    for size in SIZES:
        buffer = BytesIO()
        render(size).save(buffer, format="PNG")
        payloads.append(buffer.getvalue())
    offset = 6 + 16 * len(SIZES)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    for size, payload in zip(SIZES, payloads):
        dimension = size if size < 256 else 0
        directory.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(payload), offset))
        offset += len(payload)
    (ASSETS / "kaleidowall.ico").write_bytes(bytes(directory) + b"".join(payloads))
    print(f"Generated PNG and ICO ({', '.join(map(str, SIZES))} px) in {ASSETS}")


if __name__ == "__main__":
    main()
