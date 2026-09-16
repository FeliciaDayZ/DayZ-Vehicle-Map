package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
)

const (
	roadAsphalt = 0
	roadDirt    = 1
)

type segment struct {
	x1, z1 float32
	x2, z2 float32
	kind   byte
}

type roadLink struct {
	positions [][2]float32
	path      string
}

type routeJSON struct {
	Schema    int       `json:"schema"`
	MapID     string    `json:"mapId"`
	WorldSize float32   `json:"worldSize"`
	Nodes     []float32 `json:"nodes"`
	Edges     []int     `json:"edges"`
}

func u16(data []byte, off int) (uint16, bool) {
	if off < 0 || off+2 > len(data) {
		return 0, false
	}
	return binary.LittleEndian.Uint16(data[off : off+2]), true
}

func u32(data []byte, off int) (uint32, bool) {
	if off < 0 || off+4 > len(data) {
		return 0, false
	}
	return binary.LittleEndian.Uint32(data[off : off+4]), true
}

func f32(data []byte, off int) (float32, bool) {
	v, ok := u32(data, off)
	if !ok {
		return 0, false
	}
	f := math.Float32frombits(v)
	return f, !float32Invalid(f)
}

func float32Invalid(v float32) bool {
	return math.IsNaN(float64(v)) || math.IsInf(float64(v), 0) || math.Abs(float64(v)) > 1e7
}

func printablePathByte(v byte) bool {
	return v >= 32 && v <= 126
}

func readPath(data []byte, off int) (string, int, bool) {
	if off < 0 || off >= len(data) {
		return "", off, false
	}
	end := off
	for end < len(data) && end-off <= 512 && data[end] != 0 {
		if !printablePathByte(data[end]) {
			return "", off, false
		}
		end++
	}
	if end == off || end >= len(data) || end-off > 512 {
		return "", off, false
	}
	path := string(data[off:end])
	if !strings.HasSuffix(strings.ToLower(path), ".p3d") {
		return "", off, false
	}
	return path, end + 1, true
}

func parseRoadLink(data []byte, off int, version int) (roadLink, int, bool) {
	countValue, ok := u16(data, off)
	if !ok || countValue > 32 {
		return roadLink{}, off, false
	}
	count := int(countValue)
	off += 2
	link := roadLink{positions: make([][2]float32, count)}
	for i := 0; i < count; i++ {
		x, xok := f32(data, off)
		_, yok := f32(data, off+4)
		z, zok := f32(data, off+8)
		if !xok || !yok || !zok {
			return roadLink{}, off, false
		}
		link.positions[i] = [2]float32{x, z}
		off += 12
	}
	if version >= 24 {
		if off+count > len(data) {
			return roadLink{}, off, false
		}
		off += count
	}
	if off+4 > len(data) {
		return roadLink{}, off, false
	}
	off += 4
	if version >= 28 {
		if off+4 > len(data) {
			return roadLink{}, off, false
		}
		off += 4
	}
	if version >= 16 {
		var pathOK bool
		link.path, off, pathOK = readPath(data, off)
		if !pathOK {
			return roadLink{}, off, false
		}
		for i := 0; i < 12; i++ {
			if _, matrixOK := f32(data, off+i*4); !matrixOK {
				return roadLink{}, off, false
			}
		}
		off += 48
	}
	return link, off, true
}

func findFirstRecordCandidates(data []byte, version int) []int {
	needle := []byte(".p3d\x00")
	result := make([]int, 0, 64)
	searchAt := 0
	for {
		rel := bytes.Index(data[searchAt:], needle)
		if rel < 0 {
			break
		}
		pathEnd := searchAt + rel + len(needle)
		pathStart := pathEnd - len(needle)
		for pathStart > 0 && printablePathByte(data[pathStart-1]) {
			pathStart--
		}
		for count := 1; count <= 32; count++ {
			extraIDBytes := 0
			if version >= 28 {
				extraIDBytes = 4
			}
			recordStart := pathStart - (2 + count*12 + count + 4 + extraIDBytes)
			if recordStart < 4 {
				continue
			}
			storedCount, ok := u16(data, recordStart)
			if !ok || int(storedCount) != count {
				continue
			}
			_, next, valid := parseRoadLink(data, recordStart, version)
			if valid && next == pathEnd+48 {
				cellLinks, cellOK := u32(data, recordStart-4)
				if cellOK && cellLinks > 0 && cellLinks <= 256 {
					result = append(result, recordStart)
				}
			}
		}
		searchAt = pathEnd
	}
	sort.Ints(result)
	return result
}

func parseRoadnet(data []byte, start int, byteSize uint32, expectedCells int, version int, debug bool) ([]roadLink, int, int, bool) {
	limit := start + int(byteSize)
	if start < 0 || limit < start || limit > len(data) {
		return nil, start, 0, false
	}
	off := start
	links := make([]roadLink, 0, int(byteSize)/100)
	cell := 0
	for (expectedCells > 0 && cell < expectedCells) || (expectedCells == 0 && off < limit) {
		if off+4 > limit {
			return nil, off, cell, false
		}
		countValue, ok := u32(data, off)
		if !ok || countValue > 256 {
			if debug {
				fmt.Printf("invalid cell count cell=%d off=%d value=%d\n", cell, off, countValue)
			}
			return nil, off, cell, false
		}
		off += 4
		for i := uint32(0); i < countValue; i++ {
			link, next, linkOK := parseRoadLink(data, off, version)
			if !linkOK || next > limit {
				if debug {
					end := off + 64
					if end > len(data) {
						end = len(data)
					}
					fmt.Printf("invalid link cell=%d link=%d/%d off=%d bytes=% x\n",
						cell, i, countValue, off, data[off:end])
				}
				return nil, off, cell, false
			}
			links = append(links, link)
			off = next
		}
		cell++
	}
	valid := off == limit
	if valid && expectedCells == 0 {
		side := int(math.Sqrt(float64(cell)))
		valid = side > 0 && side*side == cell
	}
	return links, off, cell, valid
}

func locateRoadnet(data []byte, expectedCells, version int) ([]roadLink, int, int, uint32, error) {
	candidates := findFirstRecordCandidates(data, version)
	if len(candidates) > 0 {
		fmt.Printf("road-link candidates=%d first=%d last=%d\n", len(candidates), candidates[0], candidates[len(candidates)-1])
	}
	printed := 0
	for _, recordStart := range candidates {
		cellCountOff := recordStart - 4
		start := cellCountOff
		for start >= 4 {
			value, ok := u32(data, start-4)
			if !ok || value != 0 {
				break
			}
			start -= 4
		}
		if start < 4 {
			continue
		}
		byteSize, ok := u32(data, start-4)
		if !ok || byteSize < 16 || int64(start)+int64(byteSize) > int64(len(data)) {
			continue
		}
		debug := byteSize > 1000 && printed < 12
		links, end, cells, valid := parseRoadnet(data, start, byteSize, expectedCells, version, debug)
		if byteSize > 1000 && printed < 12 {
			fmt.Printf("candidate record=%d start=%d size=%d stopped=%d valid=%v\n",
				recordStart, start, byteSize, end, valid)
			printed++
		}
		if valid {
			return links, end, cells, byteSize, nil
		}
	}
	return nil, 0, 0, 0, errors.New("could not locate a valid road-network block")
}

func classifyPath(path string) (byte, bool) {
	p := strings.ToLower(strings.ReplaceAll(path, "/", "\\"))
	if strings.Contains(p, "sidewalk") || strings.Contains(p, "decal") ||
		strings.Contains(p, "roadblock") || strings.Contains(p, "road_damage") {
		return 0, false
	}
	if strings.Contains(p, "grav") || strings.Contains(p, "mud") ||
		strings.Contains(p, "dirt") || strings.Contains(p, "trail") ||
		strings.Contains(p, "track") || strings.Contains(p, "path_") {
		return roadDirt, true
	}
	if strings.Contains(p, "asf") || strings.Contains(p, "asphalt") ||
		strings.Contains(p, "tarmac") || strings.Contains(p, "beton") ||
		strings.Contains(p, "concrete") || strings.Contains(p, "panel") ||
		strings.Contains(p, "road") {
		return roadAsphalt, true
	}
	return 0, false
}

func segmentsFromLinks(links []roadLink, worldSize float32) ([]segment, map[string]int) {
	segments := make([]segment, 0, len(links))
	unknown := make(map[string]int)
	for _, link := range links {
		kind, include := classifyPath(link.path)
		if !include {
			unknown[strings.ToLower(link.path)]++
			continue
		}
		for i := 1; i < len(link.positions); i++ {
			a := link.positions[i-1]
			b := link.positions[i]
			if a[0] < -1 || a[1] < -1 || b[0] < -1 || b[1] < -1 ||
				a[0] > worldSize+1 || a[1] > worldSize+1 ||
				b[0] > worldSize+1 || b[1] > worldSize+1 {
				continue
			}
			if a != b {
				segments = append(segments, segment{a[0], a[1], b[0], b[1], kind})
			}
		}
	}
	return segments, unknown
}

func exportWRP(input, output string, worldSize float32) error {
	data, err := os.ReadFile(input)
	if err != nil {
		return err
	}
	if len(data) < 40 || string(data[:4]) != "OPRW" {
		return errors.New("not an OPRW file")
	}
	version := int(binary.LittleEndian.Uint32(data[4:8]))
	off := 8
	if version >= 28 {
		off += 4
	}
	if version >= 25 {
		off += 4
	}
	if off+20 > len(data) {
		return errors.New("truncated OPRW header")
	}
	landX := int(binary.LittleEndian.Uint32(data[off : off+4]))
	landY := int(binary.LittleEndian.Uint32(data[off+4 : off+8]))
	cellSize := math.Float32frombits(binary.LittleEndian.Uint32(data[off+16 : off+20]))
	expectedCells := 0
	if landX > 0 && landY > 0 && landX <= 8192 && landY <= 8192 {
		expectedCells = landX * landY
	}
	links, roadnetEnd, roadnetCells, byteSize, err := locateRoadnet(data, expectedCells, version)
	if err != nil {
		return err
	}
	if expectedCells == 0 {
		landX = int(math.Sqrt(float64(roadnetCells)))
		landY = landX
	}
	segments, unknown := segmentsFromLinks(links, worldSize)
	fmt.Printf("OPRW v%d land=%dx%d cell=%.3f roadnet=%d bytes end=%d links=%d segments=%d\n",
		version, landX, landY, cellSize, byteSize, roadnetEnd, len(links), len(segments))
	if len(unknown) != 0 {
		type pair struct {
			name  string
			count int
		}
		items := make([]pair, 0, len(unknown))
		for name, count := range unknown {
			items = append(items, pair{name, count})
		}
		sort.Slice(items, func(i, j int) bool { return items[i].count > items[j].count })
		fmt.Println("excluded/unclassified roadnet paths:")
		for _, item := range items {
			fmt.Printf("%7d %s\n", item.count, item.name)
		}
	}
	return writeRoadFile(output, worldSize, segments)
}

func exportRouteJSON(input, output string) error {
	data, err := os.ReadFile(input)
	if err != nil {
		return err
	}
	var graph routeJSON
	if err = json.Unmarshal(data, &graph); err != nil {
		return err
	}
	if graph.Schema != 1 || graph.WorldSize <= 0 || len(graph.Nodes)%2 != 0 || len(graph.Edges)%3 != 0 {
		return errors.New("unsupported or malformed route JSON")
	}
	nodeCount := len(graph.Nodes) / 2
	touches := make([][2]int, nodeCount)
	for i := 0; i < len(graph.Edges); i += 3 {
		a, b, kind := graph.Edges[i], graph.Edges[i+1], graph.Edges[i+2]
		if a < 0 || b < 0 || a >= nodeCount || b >= nodeCount || kind < 0 || kind > 3 {
			return errors.New("route JSON contains an invalid edge")
		}
		if kind == 0 || kind == 1 {
			touches[a][kind]++
			touches[b][kind]++
		}
	}
	segments := make([]segment, 0, len(graph.Edges)/3)
	for i := 0; i < len(graph.Edges); i += 3 {
		a, b, sourceKind := graph.Edges[i], graph.Edges[i+1], graph.Edges[i+2]
		kind := sourceKind
		include := sourceKind == 0 || sourceKind == 1
		if sourceKind == 3 {
			asphalt := touches[a][0] + touches[b][0]
			dirt := touches[a][1] + touches[b][1]
			if asphalt > 0 || dirt > 0 {
				include = true
				if dirt > asphalt {
					kind = roadDirt
				} else {
					kind = roadAsphalt
				}
			}
		}
		if include {
			segments = append(segments, segment{
				graph.Nodes[a*2], graph.Nodes[a*2+1],
				graph.Nodes[b*2], graph.Nodes[b*2+1], byte(kind),
			})
		}
	}
	fmt.Printf("route JSON map=%s nodes=%d edges=%d exported segments=%d\n",
		graph.MapID, nodeCount, len(graph.Edges)/3, len(segments))
	return writeRoadFile(output, graph.WorldSize, segments)
}

func writeRoadFile(path string, worldSize float32, segments []segment) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()
	if _, err = file.Write([]byte("DVMROAD1")); err != nil {
		return err
	}
	if err = binary.Write(file, binary.LittleEndian, uint32(len(segments))); err != nil {
		return err
	}
	if err = binary.Write(file, binary.LittleEndian, worldSize); err != nil {
		return err
	}
	for _, s := range segments {
		values := [4]float32{s.x1, s.z1, s.x2, s.z2}
		if err = binary.Write(file, binary.LittleEndian, values); err != nil {
			return err
		}
		if _, err = file.Write([]byte{s.kind, 0, 0, 0}); err != nil {
			return err
		}
	}
	return file.Close()
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage:")
	fmt.Fprintln(os.Stderr, "  wrp_road_export wrp INPUT.wrp OUTPUT.roads WORLD_SIZE")
	fmt.Fprintln(os.Stderr, "  wrp_road_export json INPUT.json OUTPUT.roads")
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "wrp":
		if len(os.Args) != 5 {
			usage()
			os.Exit(2)
		}
		var world float64
		world, err = strconv.ParseFloat(os.Args[4], 32)
		if err == nil {
			err = exportWRP(os.Args[2], os.Args[3], float32(world))
		}
	case "json":
		if len(os.Args) != 4 {
			usage()
			os.Exit(2)
		}
		err = exportRouteJSON(os.Args[2], os.Args[3])
	default:
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
