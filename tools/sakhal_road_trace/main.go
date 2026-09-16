package main

import (
	"encoding/binary"
	"fmt"
	_ "golang.org/x/image/webp"
	"image"
	"io"
	"math"
	"net/http"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

const (
	tileSize  = 256
	zoom      = 6
	tileCount = 1 << zoom
	mapPixels = tileSize * tileCount
	worldSize = float32(15360)

	roadAsphalt = byte(1)
	roadDirt    = byte(2)
)

type point struct {
	x int
	y int
}

type segment struct {
	x1, z1 float32
	x2, z2 float32
	kind   byte
}

func clamp(v float64) float64 {
	if v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}

func classifyPixel(r16, g16, b16 uint32) byte {
	r := float64(r16 >> 8)
	g := float64(g16 >> 8)
	b := float64(b16 >> 8)
	yellow := clamp((r-130)/90) * clamp((g-110)/85) * clamp((120-b)/90) *
		clamp((math.Min(r, g)-b-6)/80)
	if yellow >= .16 {
		return roadAsphalt
	}
	if r >= 55 && r <= 195 && g >= 25 && g <= 150 && b <= 110 &&
		r-g >= 12 && g-b >= 5 && r-b >= 28 {
		return roadDirt
	}
	return 0
}

func fetchTile(client *http.Client, tileX, tileY int) (image.Image, error) {
	url := fmt.Sprintf("https://static.xam.nu/dayz/maps/sakhal/1.27/topographic/%d/%d/%d.webp",
		zoom, tileX, tileY)
	var lastErr error
	for attempt := 0; attempt < 3; attempt++ {
		req, err := http.NewRequest(http.MethodGet, url, nil)
		if err != nil {
			return nil, err
		}
		req.Header.Set("User-Agent", "DayZVehicleMap build-time road extractor")
		resp, err := client.Do(req)
		if err == nil && resp.StatusCode == http.StatusOK {
			img, _, decodeErr := image.Decode(io.LimitReader(resp.Body, 4*1024*1024))
			resp.Body.Close()
			if decodeErr == nil {
				return img, nil
			}
			lastErr = decodeErr
		} else {
			if err != nil {
				lastErr = err
			} else {
				lastErr = fmt.Errorf("HTTP %s", resp.Status)
				resp.Body.Close()
			}
		}
		time.Sleep(time.Duration(attempt+1) * 200 * time.Millisecond)
	}
	return nil, fmt.Errorf("%s: %w", url, lastErr)
}

func downloadMask() ([]byte, error) {
	mask := make([]byte, mapPixels*mapPixels)
	jobs := make(chan int)
	errs := make(chan error, 1)
	client := &http.Client{Timeout: 30 * time.Second}
	var workers sync.WaitGroup
	var finished atomic.Int32
	for worker := 0; worker < 12; worker++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for job := range jobs {
				tileX := job % tileCount
				tileY := job / tileCount
				img, err := fetchTile(client, tileX, tileY)
				if err != nil {
					select {
					case errs <- err:
					default:
					}
					continue
				}
				bounds := img.Bounds()
				if bounds.Dx() != tileSize || bounds.Dy() != tileSize {
					select {
					case errs <- fmt.Errorf("tile %d/%d has dimensions %dx%d", tileX, tileY, bounds.Dx(), bounds.Dy()):
					default:
					}
					continue
				}
				baseX := tileX * tileSize
				baseY := tileY * tileSize
				for y := 0; y < tileSize; y++ {
					row := (baseY+y)*mapPixels + baseX
					for x := 0; x < tileSize; x++ {
						r, g, b, _ := img.At(bounds.Min.X+x, bounds.Min.Y+y).RGBA()
						mask[row+x] = classifyPixel(r, g, b)
					}
				}
				done := finished.Add(1)
				if done%256 == 0 {
					fmt.Printf("downloaded %d/%d tiles\n", done, tileCount*tileCount)
				}
			}
		}()
	}
	go func() {
		for job := 0; job < tileCount*tileCount; job++ {
			jobs <- job
		}
		close(jobs)
	}()
	workers.Wait()
	select {
	case err := <-errs:
		return nil, err
	default:
	}
	return mask, nil
}

func neighbors(mask []byte, idx int, result *[8]int) int {
	x := idx % mapPixels
	y := idx / mapPixels
	count := 0
	for dy := -1; dy <= 1; dy++ {
		for dx := -1; dx <= 1; dx++ {
			if dx == 0 && dy == 0 {
				continue
			}
			nx, ny := x+dx, y+dy
			if nx < 0 || ny < 0 || nx >= mapPixels || ny >= mapPixels {
				continue
			}
			next := ny*mapPixels + nx
			if mask[next]&3 == 0 {
				continue
			}
			if dx != 0 && dy != 0 &&
				(mask[y*mapPixels+nx]&3 != 0 || mask[ny*mapPixels+x]&3 != 0) {
				continue
			}
			result[count] = next
			count++
		}
	}
	return count
}

func foregroundIndices(mask []byte) []int {
	indices := make([]int, 0, len(mask)/100)
	for idx, value := range mask {
		if value != 0 {
			indices = append(indices, idx)
		}
	}
	return indices
}

func transitions(p [8]bool) int {
	count := 0
	for i := 0; i < 8; i++ {
		if !p[i] && p[(i+1)%8] {
			count++
		}
	}
	return count
}

func thinningSubstep(mask []byte, active []int, second bool) int {
	remove := make([]int, 0, len(active)/20)
	for _, idx := range active {
		if mask[idx]&3 == 0 {
			continue
		}
		x := idx % mapPixels
		y := idx / mapPixels
		if x == 0 || y == 0 || x == mapPixels-1 || y == mapPixels-1 {
			continue
		}
		p := [8]bool{
			mask[idx-mapPixels]&3 != 0,
			mask[idx-mapPixels+1]&3 != 0,
			mask[idx+1]&3 != 0,
			mask[idx+mapPixels+1]&3 != 0,
			mask[idx+mapPixels]&3 != 0,
			mask[idx+mapPixels-1]&3 != 0,
			mask[idx-1]&3 != 0,
			mask[idx-mapPixels-1]&3 != 0,
		}
		around := 0
		for _, present := range p {
			if present {
				around++
			}
		}
		if around < 2 || around > 6 || transitions(p) != 1 {
			continue
		}
		if !second {
			if p[0] && p[2] && p[4] || p[2] && p[4] && p[6] {
				continue
			}
		} else if p[0] && p[2] && p[6] || p[0] && p[4] && p[6] {
			continue
		}
		remove = append(remove, idx)
	}
	for _, idx := range remove {
		mask[idx] = 0
	}
	return len(remove)
}

func thinMask(mask []byte, active []int) {
	for iteration := 1; ; iteration++ {
		removed := thinningSubstep(mask, active, false)
		removed += thinningSubstep(mask, active, true)
		fmt.Printf("thinning pass %d removed %d pixels\n", iteration, removed)
		if removed == 0 {
			return
		}
		if iteration%8 == 0 {
			kept := active[:0]
			for _, idx := range active {
				if mask[idx]&3 != 0 {
					kept = append(kept, idx)
				}
			}
			active = kept
		}
	}
}

func removeSmallComponents(mask []byte, active []int) int {
	removed := 0
	queue := make([]int, 0, 4096)
	component := make([]int, 0, 4096)
	for _, seed := range active {
		if mask[seed]&3 == 0 || mask[seed]&0x80 != 0 {
			continue
		}
		queue = append(queue[:0], seed)
		component = component[:0]
		mask[seed] |= 0x80
		asphalt := 0
		minX, maxX := seed%mapPixels, seed%mapPixels
		minY, maxY := seed/mapPixels, seed/mapPixels
		for len(queue) != 0 {
			idx := queue[len(queue)-1]
			queue = queue[:len(queue)-1]
			component = append(component, idx)
			if mask[idx]&3 == roadAsphalt {
				asphalt++
			}
			x, y := idx%mapPixels, idx/mapPixels
			if x < minX {
				minX = x
			}
			if x > maxX {
				maxX = x
			}
			if y < minY {
				minY = y
			}
			if y > maxY {
				maxY = y
			}
			var adjacent [8]int
			count := neighbors(mask, idx, &adjacent)
			for i := 0; i < count; i++ {
				next := adjacent[i]
				if mask[next]&3 != 0 && mask[next]&0x80 == 0 {
					mask[next] |= 0x80
					queue = append(queue, next)
				}
			}
		}
		tooSmall := len(component) < 20 || (maxX-minX < 24 && maxY-minY < 24)
		for _, idx := range component {
			if tooSmall {
				mask[idx] = 0
				removed++
			}
		}
	}
	for _, idx := range active {
		mask[idx] &= 3
	}
	return removed
}

func canonicalEdge(a, b int) (int, int, bool) {
	if b < a {
		a, b = b, a
	}
	delta := b - a
	code := -1
	switch delta {
	case 1:
		code = 0
	case mapPixels - 1:
		code = 1
	case mapPixels:
		code = 2
	case mapPixels + 1:
		code = 3
	}
	if code < 0 {
		return 0, 0, false
	}
	bit := a*4 + code
	return bit >> 3, bit & 7, true
}

func edgeSeen(bits []byte, a, b int) bool {
	index, shift, ok := canonicalEdge(a, b)
	return ok && bits[index]&(1<<shift) != 0
}

func markEdge(bits []byte, a, b int) {
	index, shift, ok := canonicalEdge(a, b)
	if ok {
		bits[index] |= 1 << shift
	}
}

func pointLineDistance(p, a, b point) float64 {
	dx := float64(b.x - a.x)
	dy := float64(b.y - a.y)
	if dx == 0 && dy == 0 {
		return math.Hypot(float64(p.x-a.x), float64(p.y-a.y))
	}
	t := (float64(p.x-a.x)*dx + float64(p.y-a.y)*dy) / (dx*dx + dy*dy)
	projectionX := float64(a.x) + t*dx
	projectionY := float64(a.y) + t*dy
	return math.Hypot(float64(p.x)-projectionX, float64(p.y)-projectionY)
}

func simplify(points []point, tolerance float64) []point {
	if len(points) <= 2 {
		return points
	}
	keep := make([]bool, len(points))
	keep[0], keep[len(points)-1] = true, true
	type interval struct{ first, last int }
	stack := []interval{{0, len(points) - 1}}
	for len(stack) != 0 {
		span := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		farthest := -1
		maxDistance := tolerance
		for i := span.first + 1; i < span.last; i++ {
			distance := pointLineDistance(points[i], points[span.first], points[span.last])
			if distance > maxDistance {
				maxDistance = distance
				farthest = i
			}
		}
		if farthest >= 0 {
			keep[farthest] = true
			stack = append(stack, interval{span.first, farthest}, interval{farthest, span.last})
		}
	}
	result := make([]point, 0, len(points))
	for i, p := range points {
		if keep[i] {
			result = append(result, p)
		}
	}
	return result
}

func traceChain(mask, edgeBits []byte, start, next int) ([]point, byte) {
	indices := make([]int, 0, 128)
	indices = append(indices, start)
	previous := start
	current := next
	markEdge(edgeBits, previous, current)
	for {
		indices = append(indices, current)
		if current == start {
			break
		}
		var adjacent [8]int
		count := neighbors(mask, current, &adjacent)
		if count != 2 {
			break
		}
		candidate := adjacent[0]
		if candidate == previous {
			candidate = adjacent[1]
		}
		if edgeSeen(edgeBits, current, candidate) {
			break
		}
		markEdge(edgeBits, current, candidate)
		previous, current = current, candidate
	}
	asphalt, dirt := 0, 0
	points := make([]point, len(indices))
	for i, idx := range indices {
		if mask[idx]&3 == roadAsphalt {
			asphalt++
		} else {
			dirt++
		}
		points[i] = point{idx % mapPixels, idx / mapPixels}
	}
	kind := byte(0)
	if dirt > asphalt {
		kind = 1
	}
	return simplify(points, 2.0), kind
}

func appendChainSegments(result []segment, points []point, kind byte) []segment {
	if len(points) < 2 {
		return result
	}
	metersPerPixel := worldSize / float32(mapPixels)
	for i := 1; i < len(points); i++ {
		a, b := points[i-1], points[i]
		result = append(result, segment{
			(float32(a.x) + .5) * metersPerPixel,
			worldSize - (float32(a.y)+.5)*metersPerPixel,
			(float32(b.x) + .5) * metersPerPixel,
			worldSize - (float32(b.y)+.5)*metersPerPixel,
			kind,
		})
	}
	return result
}

func traceSegments(mask []byte, active []int) []segment {
	edgeBits := make([]byte, (len(mask)*4+7)/8)
	segments := make([]segment, 0, len(active)/2)
	for _, idx := range active {
		if mask[idx]&3 == 0 {
			continue
		}
		var adjacent [8]int
		count := neighbors(mask, idx, &adjacent)
		if count == 2 {
			continue
		}
		for i := 0; i < count; i++ {
			next := adjacent[i]
			if edgeSeen(edgeBits, idx, next) {
				continue
			}
			points, kind := traceChain(mask, edgeBits, idx, next)
			segments = appendChainSegments(segments, points, kind)
		}
	}
	for _, idx := range active {
		if mask[idx]&3 == 0 {
			continue
		}
		var adjacent [8]int
		count := neighbors(mask, idx, &adjacent)
		for i := 0; i < count; i++ {
			next := adjacent[i]
			if edgeSeen(edgeBits, idx, next) {
				continue
			}
			points, kind := traceChain(mask, edgeBits, idx, next)
			segments = appendChainSegments(segments, points, kind)
		}
	}
	return segments
}

func writeRoadFile(path string, segments []segment) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	if _, err = file.Write([]byte("DVMROAD1")); err != nil {
		file.Close()
		return err
	}
	if err = binary.Write(file, binary.LittleEndian, uint32(len(segments))); err != nil {
		file.Close()
		return err
	}
	if err = binary.Write(file, binary.LittleEndian, worldSize); err != nil {
		file.Close()
		return err
	}
	for _, s := range segments {
		if err = binary.Write(file, binary.LittleEndian, [4]float32{s.x1, s.z1, s.x2, s.z2}); err != nil {
			file.Close()
			return err
		}
		if _, err = file.Write([]byte{s.kind, 0, 0, 0}); err != nil {
			file.Close()
			return err
		}
	}
	return file.Close()
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: sakhal_road_trace OUTPUT.roads")
		os.Exit(2)
	}
	mask, err := downloadMask()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	active := foregroundIndices(mask)
	sort.Ints(active)
	fmt.Printf("classified %d foreground pixels\n", len(active))
	thinMask(mask, active)
	removed := removeSmallComponents(mask, active)
	fmt.Printf("removed %d small map-symbol component pixels\n", removed)
	segments := traceSegments(mask, active)
	if err = writeRoadFile(os.Args[1], segments); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("wrote %d Sakhal road segments to %s\n", len(segments), os.Args[1])
}
