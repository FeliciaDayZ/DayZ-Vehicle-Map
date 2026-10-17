"""Offline source-conversion and native rendering regression checks."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

import build_nasdara_objects as builder

RENDERER = None


class SourceTests(unittest.TestCase):
    def test_static_coordinates_preserve_rotation_and_nonunit_scale(self):
        self.assertEqual(builder.decode_static_chunk("9_8", 1024, [125,250,-905,3,1250]),
                         [[9228.5,8217.0,-90.5,3,1.25]])
        with self.assertRaises(ValueError):
            builder.decode_static_chunk("0_0", 1024, [1,2,3])

    def test_real_palace_placement_matches_installed_mission(self):
        # Placement angle is CE's `a`, not its yaw: a = 90 - yaw.
        x, z = builder.transform(10142.7, 8210.0, -116.265, 1, (0, 10))
        self.assertAlmostEqual(x, 10151.667609, places=4)
        self.assertAlmostEqual(z, 8205.574759, places=4)

    def test_road_delta_decode_and_surface_type(self):
        data = {"classes":[{"id":"main"},{"id":"dirt2"}],
                "lines":[[0,0,1000,2000,100,-50,0,0],[1,0,50,60,10,20]]}
        self.assertEqual(builder.decode_roads(data), [(100,200,110,195,0),(5,6,6,8,1)])


class NativeTests(unittest.TestCase):
    def setUp(self):
        if RENDERER is None:
            self.skipTest("pass --renderer for the native rendering checks")
        self.temp = tempfile.TemporaryDirectory(prefix="dvm-object-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        pictures = self.directory / "pictures"
        pictures.mkdir()
        image = Image.new("RGBA", (8,4))
        for y in range(4):
            for x in range(8):
                image.putpixel((x,y), ((255,0,0,255), (0,255,0,255),
                                      (0,0,255,255), (255,255,0,255))[(y>=2)*2+(x>=4)])
        image.save(pictures / "0000.png")
        model = {"name":"Fixture", "rings":[[0,0,8,0,8,4,0,4]], "top":[1,0,0,1,0,0]}
        with patch.object(builder, "WORLD", 64.0):
            builder.write_objects(self.directory / "nasdara.objects", [model],
                                  [[20,20,90,0,2]], {0:(8,4)})

    def render(self, reject=False):
        run = subprocess.run([str(RENDERER)] + (["--reject"] if reject else []),
                             cwd=self.directory, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_native_rotation_scale_and_zoom(self):
        self.render()

    def test_truncated_asset_rejected(self):
        path = self.directory / "nasdara.objects"
        path.write_bytes(path.read_bytes()[:-1])
        self.render(reject=True)

    def test_invalid_class_reference_rejected(self):
        path = self.directory / "nasdara.objects"
        data = bytearray(path.read_bytes())
        struct.pack_into("<I", data, len(data)-20, 99)
        path.write_bytes(data)
        self.render(reject=True)

    def test_nonfinite_placement_rejected(self):
        path = self.directory / "nasdara.objects"
        data = bytearray(path.read_bytes())
        struct.pack_into("<f", data, len(data)-16, float("nan"))
        path.write_bytes(data)
        self.render(reject=True)

    def test_missing_picture_rejected(self):
        (self.directory / "pictures/0000.png").unlink()
        self.render(reject=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path)
    args, rest = parser.parse_known_args()
    RENDERER = args.renderer.resolve() if args.renderer else None
    unittest.main(argv=[__file__]+rest)
