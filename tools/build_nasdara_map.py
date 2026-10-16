"""Build the calibrated Nasdara satellite image from the installed DayZ PBOs.

Requires Python 3, Pillow, and DayZ Tools (ImageToPAA and CfgConvert).
Only reads the game installation; extracted assets stay under --temp.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import subprocess

from PIL import Image


def read_string(stream):
    value = bytearray()
    while True:
        char = stream.read(1)
        if not char:
            raise ValueError("Truncated PBO header")
        if char == b"\0":
            return value.decode("utf-8")
        value.extend(char)


def extract_selected(archive, destination, select):
    """Read uncompressed PBO entries, rejecting unsupported formats and paths."""
    entries = []
    with archive.open("rb") as stream:
        while True:
            name = read_string(stream)
            method, _, _, _, size = struct.unpack("<5I", stream.read(20))
            if not name:
                if method == 0x56657273:
                    while read_string(stream):
                        read_string(stream)
                    continue
                break
            if method != 0:
                raise ValueError(f"Unsupported PBO compression: {name}")
            relative = PureWindowsPath(name)
            if relative.is_absolute() or relative.drive or ".." in relative.parts:
                raise ValueError(f"Unsafe PBO path: {name}")
            entries.append((name, relative, size))
        offset = stream.tell()
        for name, relative, size in entries:
            if select(name.lower()):
                output = destination.joinpath(*relative.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                stream.seek(offset)
                data = stream.read(size)
                if len(data) != size:
                    raise ValueError(f"Truncated PBO entry: {name}")
                output.write_bytes(data)
            offset += size


def convert(command, output):
    result = subprocess.run(command, capture_output=True, text=True,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode or not output.is_file():
        raise RuntimeError(f"Conversion failed: {command}\n{result.stdout}\n{result.stderr}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--addons", type=Path, required=True)
    parser.add_argument("--dayz-tools", type=Path, required=True,
                        help="DayZ Tools directory containing Bin")
    parser.add_argument("--temp", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    data = args.temp / "worlds_nasdara_data"
    ce = args.temp / "worlds_nasdara_ce"
    converter = args.dayz_tools / "Bin/ImageToPAA/ImageToPAA.exe"
    cfgconvert = args.dayz_tools / "Bin/CfgConvert/CfgConvert.exe"
    for executable in (converter, cfgconvert):
        if not executable.is_file():
            raise FileNotFoundError(executable)

    materials = [f"layers/p_{x:03}-{y:03}_l05_l09_l10_l16.rvmat"
                 for x, y in [(0, 0), (1, 0), (0, 1)]]
    extract_selected(args.addons / "worlds_nasdara_data.pbo", data,
                     lambda name: name.startswith("layers\\s_") or
                     name.replace("\\", "/") in materials)
    extract_selected(args.addons / "worlds_nasdara_ce.pbo", ce,
                     lambda name: name in ("cfgeventspawns.xml", "db\\events.xml", "areaflags.map"))
    with (ce / "areaflags.map").open("rb") as stream:
        width, height, world_x, world_z, _ = struct.unpack("<5I", stream.read(20))
    if (width, height, world_x, world_z) != (4096, 4096, 16384, 16384):
        raise ValueError("Nasdara terrain extent changed; recalibrate before regenerating")



    for material, position in zip(materials, [(0.125, 32.125, 0),
                                              (-0.625, 32.125, 0),
                                              (0.125, 31.375, 0)]):
        decoded = args.temp / (Path(material).stem + ".cpp")
        convert([str(cfgconvert), "-txt", "-dst", str(decoded), str(data / material)], decoded)
        text = decoded.read_text()
        block = re.search(r"class TexGen3\s*\{(.*?)\n\};", text, re.S).group(1)
        for key, expected in [("aside", (1 / 512, 0, 0)),
                              ("up", (0, 0, 1 / 512)),
                              ("dir", (0, -1 / 512, 0)), ("pos", position)]:
            values = re.search(rf"{key}\[\]\s*=\s*\{{([^}}]+)\}}", block).group(1)
            if tuple(float(v) for v in values.split(",")) != expected:
                raise ValueError(f"Terrain material {material}: unexpected {key}")

    tiles = sorted((data / "layers").glob("[sS]_???_???_lco.paa"))
    if len(tiles) != 43 * 43:
        raise ValueError(f"Expected 43 x 43 satellite tiles; found {len(tiles)}")
    converted = args.temp / "satellite_png"
    converted.mkdir(parents=True, exist_ok=True)

    def prepare_tile(tile):
        output = converted / (tile.stem + ".png")
        if not output.exists() or output.stat().st_mtime < tile.stat().st_mtime:
            convert([str(converter), str(tile), str(output)], output)
        _, x, y, _ = tile.stem.split("_")
        with Image.open(output) as image:
            if image.size != (512, 512):
                raise ValueError(f"Unexpected tile size: {tile}")
            image = image.convert("RGB").resize((128, 128), Image.Resampling.LANCZOS)
            return int(x), int(y), image.crop((16, 16, 112, 112))

    mosaic = Image.new("RGB", (4096, 4096))
    with ThreadPoolExecutor(max_workers=4) as executor:
        for count, (x, y, tile) in enumerate(executor.map(prepare_tile, tiles), 1):
            if not (0 <= x < 43 and 0 <= y < 43):
                raise ValueError("Satellite tile outside verified grid")
            mosaic.paste(tile, (x * 96, y * 96))
            if count % 200 == 0:
                print(f"Assembled {count}/{len(tiles)} tiles", flush=True)

    output = args.project / "data/maps/nasdara.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    mosaic.save(output)
    source = args.project / "data/source/nasdara"
    source.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ce / "cfgeventspawns.xml", source / "cfgeventspawns.xml")
    shutil.copyfile(ce / "db/events.xml", source / "events.xml")
    print(f"Built {output}: 4096 x 4096, 16384 m world, north up", flush=True)


if __name__ == "__main__":
    main()
