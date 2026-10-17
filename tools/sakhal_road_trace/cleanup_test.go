package main

import (
	"image/png"
	"os"
	"testing"
)

func testMask() []byte {
	// Keep the production row stride, with enough rows for these local fixtures.
	return make([]byte, mapPixels*256)
}

func paintRect(mask []byte, x0, y0, x1, y1 int, kind byte) {
	for y := y0; y < y1; y++ {
		for x := x0; x < x1; x++ {
			mask[y*mapPixels+x] = kind
		}
	}
}

func TestFillSmallHoles(t *testing.T) {
	for _, kind := range []byte{roadAsphalt, roadDirt} {
		mask := testMask()
		paintRect(mask, 20, 20, 100, 60, kind)
		paintRect(mask, 30, 30, 32, 32, 0) // Compression pinhole.
		paintRect(mask, 40, 30, 41, 42, 0) // Thin crack, still only 12 pixels.
		paintRect(mask, 70, 30, 80, 40, 0) // A real island must survive.
		paintRect(mask, 20, 45, 30, 46, 0) // Open to the background.
		if got := fillSmallHoles(mask); got != 16 {
			t.Fatalf("kind %d: filled %d pixels, want 16", kind, got)
		}
		for _, p := range []point{{30, 30}, {40, 35}} {
			if mask[p.y*mapPixels+p.x] != kind {
				t.Fatalf("kind %d: hole at %v was not filled with its road type", kind, p)
			}
		}
		for _, p := range []point{{75, 35}, {25, 45}, {0, 0}, {110, 40}} {
			if mask[p.y*mapPixels+p.x] != 0 {
				t.Fatalf("kind %d: background at %v was filled", kind, p)
			}
		}
		if got := fillSmallHoles(mask); got != 0 {
			t.Fatalf("cleanup is not stable: second pass filled %d pixels", got)
		}
	}
}

func TestPruneSpursPreservesRoadTopology(t *testing.T) {
	for _, kind := range []byte{roadAsphalt, roadDirt} {
		mask := testMask()
		paintRect(mask, 20, 50, 121, 51, kind)
		paintRect(mask, 40, 46, 41, 50, kind) // Four-pixel artifact.
		paintRect(mask, 70, 25, 71, 50, kind) // Real dead-end side road.
		paintRect(mask, 95, 50, 96, 76, kind) // Real bent side road.
		paintRect(mask, 95, 75, 121, 76, kind)
		paintRect(mask, 140, 50, 146, 51, kind) // Short isolated road.
		// A real closed loop.
		paintRect(mask, 160, 30, 181, 31, kind)
		paintRect(mask, 160, 50, 181, 51, kind)
		paintRect(mask, 160, 30, 161, 51, kind)
		paintRect(mask, 180, 30, 181, 51, kind)
		want := append([]byte(nil), mask...)
		paintRect(want, 40, 46, 41, 50, 0)
		active := foregroundIndices(mask)
		if got := pruneShortSpurs(mask, active, 8); got != 4 {
			t.Fatalf("kind %d: pruned %d pixels, want 4", kind, got)
		}
		for idx := range mask {
			if mask[idx] != want[idx] {
				t.Fatalf("kind %d: unexpected topology change at %d,%d", kind, idx%mapPixels, idx/mapPixels)
			}
		}
		if got := pruneShortSpurs(mask, active, 8); got != 0 {
			t.Fatalf("second pruning pass removed %d pixels", got)
		}
	}
}

func TestNasdaraCurveHasNoBranchesOrLoops(t *testing.T) {
	file, err := os.Open("testdata/nasdara_curve_mask.png")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	img, err := png.Decode(file)
	if err != nil {
		t.Fatal(err)
	}
	// Classified crop of the bend in the bug report: Nasdara 26.10.15, zoom 6,
	// image pixels (10685, 5622), size 240x105. Tests need no network or tile cache.
	// Exercise both road classes with the same real raster defects.
	for _, kind := range []byte{roadAsphalt, roadDirt} {
		mask := testMask()
		for y := 0; y < img.Bounds().Dy(); y++ {
			for x := 0; x < img.Bounds().Dx(); x++ {
				r, _, _, _ := img.At(x, y).RGBA()
				if r != 0 {
					mask[(y+20)*mapPixels+x+20] = kind
				}
			}
		}
		fillSmallHoles(mask)
		active := foregroundIndices(mask)
		thinMask(mask, active)
		removeSmallComponents(mask, active)
		pruneShortSpurs(mask, active, 8)
		endpoints := 0
		for _, idx := range foregroundIndices(mask) {
			var adjacent [8]int
			degree := neighbors(mask, idx, &adjacent)
			if degree == 1 {
				endpoints++
			} else if degree != 2 {
				t.Fatalf("kind %d: spurious junction at %d,%d (degree %d)", kind, idx%mapPixels, idx/mapPixels, degree)
			}
		}
		if endpoints != 2 {
			t.Fatalf("kind %d: got %d endpoints, want one continuous road with two ends", kind, endpoints)
		}
		remaining := foregroundIndices(mask)
		seen := map[int]bool{remaining[0]: true}
		queue := []int{remaining[0]}
		for head := 0; head < len(queue); head++ {
			var adjacent [8]int
			count := neighbors(mask, queue[head], &adjacent)
			for _, next := range adjacent[:count] {
				if !seen[next] {
					seen[next] = true
					queue = append(queue, next)
				}
			}
		}
		if len(seen) != len(remaining) {
			t.Fatalf("kind %d: disconnected geometry remains beside the road", kind)
		}
		segments := traceSegments(mask, active)
		if len(segments) < 3 || len(segments) > 10 {
			t.Fatalf("kind %d: bend should survive as a few segments, got %d", kind, len(segments))
		}
		for _, s := range segments {
			if s.kind != kind-1 {
				t.Fatalf("road type changed from %d to %d", kind, s.kind)
			}
		}
	}
}
