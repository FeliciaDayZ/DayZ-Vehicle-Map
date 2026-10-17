"""Build offline Nasdara structures and roads from DayZed.GG's model data.

Requires Python 3 and Pillow. Source files stay in --cache; the application
uses the generated binary geometry and PNG model pictures without networking.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import time
import urllib.request
import urllib.parse

from PIL import Image

SOURCE = "https://dayzed.gg/static/maps/"
WORLD = 16384.0
VERSION = "1792185360"
ROAD_VERSION = "1792185361"


def fetch(cache, relative, version):
    path = cache / relative
    if path.is_file():
        return path.read_bytes()
    for attempt in range(3):
        try:
            req = urllib.request.Request(SOURCE + relative + "?v=" + version,
                                         headers={"User-Agent": "DayZVehicleMap offline data builder"})
            with urllib.request.urlopen(req, timeout=45) as response:
                data = response.read()
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return data
        except OSError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def transform(x, z, angle, scale, point):
    radians = math.radians(angle)
    c, s = math.cos(radians) * scale, math.sin(radians) * scale
    return x + c * point[0] - s * point[1], z + s * point[0] + c * point[1]


def decode_static_chunk(key, size, flat):
    if len(flat) % 5:
        raise ValueError(f"Truncated static chunk: {key}")
    cx, cz = map(int, key.split("_"))
    return [[cx * size + flat[i] / 10, cz * size + flat[i+1] / 10,
             flat[i+2] / 10, flat[i+3], flat[i+4] / 1000]
            for i in range(0, len(flat), 5)]


def model_bounds(cls, width, height):
    points = [(ring[i], ring[i+1]) for ring in cls["rings"]
              for i in range(0, len(ring), 2)]
    if width and height:
        a, b, c, d, e, f = cls["top"]
        points += [(a*u+c*v+e, b*u+d*v+f)
                   for u, v in [(0, 0), (width, 0), (0, height), (width, height)]]
    if not points or not all(math.isfinite(n) for p in points for n in p):
        raise ValueError(f"Invalid bounds for {cls['name']}")
    xs, zs = zip(*points)
    return min(xs), min(zs), max(xs), max(zs)


def write_objects(output, classes, items, pictures):
    rings, points, records = [], [], []
    for index, cls in enumerate(classes):
        first_ring = len(rings)
        for ring in cls["rings"]:
            if len(ring) < 6 or len(ring) % 2:
                raise ValueError(f"Invalid outline: {cls['name']}")
            rings.append((len(points), len(ring)//2))
            points.extend(zip(ring[::2], ring[1::2]))
        width, height = pictures.get(index, (0, 0))
        flags = bool(cls.get("wreck")) | (bool(cls.get("s")) << 1) | (bool(width) << 2)
        records.append(struct.pack("<5I10f", first_ring, len(cls["rings"]), flags,
                                   width, height, *cls.get("top", [0]*6),
                                   *model_bounds(cls, width, height)))
    with output.open("wb") as stream:
        stream.write(struct.pack("<8sf4I", b"DVMOBJ1\0", WORLD, len(classes),
                                 len(items), len(rings), len(points)))
        for record in records:
            stream.write(record)
        for ring in rings:
            stream.write(struct.pack("<2I", *ring))
        for point in points:
            stream.write(struct.pack("<2f", *point))
        for x, z, angle, index, scale in items:
            if not (0 <= index < len(classes) and all(math.isfinite(v) for v in (x,z,angle,scale))
                    and -512 <= x <= WORLD+512 and -512 <= z <= WORLD+512 and 0 < scale <= 100):
                raise ValueError(f"Invalid placement: {(x,z,angle,index,scale)}")
            stream.write(struct.pack("<I4f", index, x, z, angle, scale))


def decode_roads(data):
    segments = []
    for line in data["lines"]:
        if len(line) < 6 or len(line) % 2:
            raise ValueError("Invalid road line")
        kind = 1 if data["classes"][line[0]]["id"].startswith(("dirt", "path")) else 0
        x, z = line[2] / 10, line[3] / 10
        for offset in range(4, len(line), 2):
            nx, nz = x + line[offset] / 10, z + line[offset+1] / 10
            # The source includes a few boundary pieces outside the map.
            a, b = min(WORLD, max(0, x)), min(WORLD, max(0, z))
            c, d = min(WORLD, max(0, nx)), min(WORLD, max(0, nz))
            if (a, b) != (c, d):
                segments.append((a, b, c, d, kind))
            x, z = nx, nz
    return segments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--roads-output", type=Path,
                        help="road file destination (default: OUTPUT/nasdara.roads)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    pictures_dir = args.output / "pictures"
    pictures_dir.mkdir(exist_ok=True)
    raw = fetch(args.cache, "nasdara/data/buildings.json", VERSION)
    data = json.loads(raw)
    if data["version"] != 2:
        raise ValueError("Unsupported building schema")
    static_raw = fetch(args.cache, "nasdara/data/statics/classes.json", VERSION)
    all_classes = data["classes"] + json.loads(static_raw)
    meta = data["statics"]
    items = [list(it[:4]) + [it[4] if len(it) > 4 else 1] for it in data["items"]]
    def chunk(key):
        flat = json.loads(fetch(args.cache, "nasdara/data/statics/"+key+".json", VERSION))
        return decode_static_chunk(key, meta["chunk"], flat)
    with ThreadPoolExecutor(max_workers=6) as pool:
        for count, batch in enumerate(pool.map(chunk, meta["chunks"]), 1):
            items.extend(batch)
            if count % 30 == 0:
                print(f"Read {count}/{len(meta['chunks'])} static chunks", flush=True)
    used = sorted({it[3] for it in items
                   if all_classes[it[3]].get("rings") and not all_classes[it[3]].get("u")})
    remap = {old: new for new, old in enumerate(used)}
    classes = [all_classes[old] for old in used]
    items = [[x,z,a,remap[c],s] for x,z,a,c,s in items if c in remap]
    def picture(entry):
        index, cls = entry
        if not cls.get("top"):
            return index, (0, 0)
        name = cls["name"].lower()
        if any(ch not in "abcdefghijklmnopqrstuvwxyz0123456789_ -" for ch in name):
            raise ValueError(f"Unsafe model name: {name}")
        image = Image.open(io.BytesIO(fetch(args.cache, "nasdara/buildings/"+
                                           urllib.parse.quote(name)+"_top.webp", VERSION)))
        image = image.convert("RGBA")
        image.save(pictures_dir / f"{index:04}.png")
        return index, image.size
    pictures = {}
    with ThreadPoolExecutor(max_workers=6) as pool:
        for count, (index, dimensions) in enumerate(pool.map(picture, enumerate(classes)), 1):
            pictures[index] = dimensions
            if count % 100 == 0:
                print(f"Prepared {count}/{len(classes)} model pictures", flush=True)
    write_objects(args.output / "nasdara.objects", classes, items, pictures)
    roads_raw = fetch(args.cache, "nasdara_topo/data/roads.json", ROAD_VERSION)
    roads = decode_roads(json.loads(roads_raw))
    roads_output = args.roads_output or args.output / "nasdara.roads"
    roads_output.parent.mkdir(parents=True, exist_ok=True)
    with roads_output.open("wb") as stream:
        stream.write(struct.pack("<8sIf", b"DVMROAD1", len(roads), WORLD))
        for road in roads:
            stream.write(struct.pack("<4fB3x", *road))
    manifest = {"source": "https://dayzed.gg/map/nasdara", "buildings_version": VERSION,
                "roads_version": ROAD_VERSION, "buildings_sha256": hashlib.sha256(raw).hexdigest(),
                "static_classes_sha256": hashlib.sha256(static_raw).hexdigest(),
                "roads_sha256": hashlib.sha256(roads_raw).hexdigest(),
                "classes": [c["name"] for c in classes], "placements": len(items),
                "road_segments": len(roads)}
    (args.output / "source.json").write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
    print(f"Built {len(items)} structures, {len(classes)} models, {len(roads)} road segments", flush=True)


if __name__ == "__main__":
    main()
