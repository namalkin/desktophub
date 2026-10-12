#!/usr/bin/env python3
"""Собирает catalog.pak — всё, что центр показывает о виджетах, одним ресурсом .exe.

В пакет попадают catalog.json, README.md каждого виджета, картинки, на которые README
ссылается, и иконка виджета (res/app.ico). Пути внутри пакета — от корня репозитория,
через «/»: «widget 5/docs/screenshot.png».

Формат: b"WPAK", u32 число записей, затем для каждой записи
u32 длина имени, имя в UTF-8, u32 длина данных, данные. Всё little-endian.

    python3 pack.py <корень репозитория> <catalog.json> <выходной .pak>
"""
import json
import re
import struct
import sys
from pathlib import Path

IMAGE_RE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)")


def main() -> int:
    repo, catalog_path, out_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    catalog_bytes = catalog_path.read_bytes()
    catalog = json.loads(catalog_bytes.decode("utf-8"))

    entries = [("catalog.json", catalog_bytes)]
    for widget in catalog["widgets"]:
        folder = widget["folder"]
        base = repo / folder

        readme = base / "README.md"
        if readme.is_file():
            text = readme.read_bytes()
            entries.append((f"{folder}/README.md", text))
            for ref in IMAGE_RE.findall(text.decode("utf-8")):
                if "://" in ref:
                    continue  # внешние картинки центр не грузит
                image = base / ref
                if image.is_file():
                    entries.append((f"{folder}/{ref}", image.read_bytes()))
                else:
                    print(f"pack: нет картинки {image}", file=sys.stderr)
        else:
            print(f"pack: нет {readme}", file=sys.stderr)

        icon = base / "res" / "app.ico"
        if icon.is_file():
            entries.append((f"{folder}/res/app.ico", icon.read_bytes()))

    with out_path.open("wb") as out:
        out.write(b"WPAK")
        out.write(struct.pack("<I", len(entries)))
        for name, data in entries:
            raw = name.encode("utf-8")
            out.write(struct.pack("<I", len(raw)))
            out.write(raw)
            out.write(struct.pack("<I", len(data)))
            out.write(data)

    total = sum(len(d) for _, d in entries)
    print(f"pack: {len(entries)} файлов, {total // 1024} КБ -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
