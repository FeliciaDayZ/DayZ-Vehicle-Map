package main

import "math"

// Compression and antialiasing can leave a few missing pixels inside a road's
// coloured fill. Thinning preserves those holes as loops. Fill only enclosed
// holes smaller than a road width; retain larger islands and roundabouts.
func fillSmallHoles(mask []byte) int {
	const maxArea = 16
	const exterior = byte(0x40)
	const visiting = byte(0x80)
	height := len(mask) / mapPixels
	filled := 0
	component := make([]int, 0, maxArea+4)
	for seed, value := range mask {
		if value != 0 {
			continue
		}
		component = append(component[:0], seed)
		mask[seed] = visiting
		asphalt, dirt := 0, 0
		open := false
		for head := 0; head < len(component) && !open; head++ {
			idx := component[head]
			x, y := idx%mapPixels, idx/mapPixels
			if x == 0 || y == 0 || x == mapPixels-1 || y == height-1 {
				open = true
				break
			}
			for _, next := range [4]int{idx - mapPixels, idx + 1, idx + mapPixels, idx - 1} {
				switch mask[next] {
				case 0:
					mask[next] = visiting
					component = append(component, next)
				case exterior:
					open = true
				case roadAsphalt:
					asphalt++
				case roadDirt:
					dirt++
				}
			}
			if len(component) > maxArea {
				open = true
			}
		}
		replacement := exterior
		if !open {
			replacement = roadAsphalt
			if dirt > asphalt {
				replacement = roadDirt
			}
			filled += len(component)
		}
		for _, idx := range component {
			mask[idx] = replacement
		}
	}
	for idx := range mask {
		mask[idx] &= 3
	}
	return filled
}

// Small bumps on a raster road edge become dead-end branches when thinned.
// Remove only short terminal chains attached to a junction, never standalone
// roads, through connections, or closed loops. Retrace after pruning so the
// former junctions can simplify into one continuous centerline.
func pruneShortSpurs(mask []byte, active []int, maxLength float64) int {
	removed := 0
	chain := make([]int, 0, int(maxLength)+2)
	for {
		remove := make([]int, 0)
		for _, seed := range active {
			if mask[seed]&3 == 0 {
				continue
			}
			var adjacent [8]int
			if neighbors(mask, seed, &adjacent) != 1 {
				continue
			}
			chain = append(chain[:0], seed)
			previous, current := seed, adjacent[0]
			length := 0.0
			for {
				length += math.Hypot(float64(current%mapPixels-previous%mapPixels),
					float64(current/mapPixels-previous/mapPixels))
				if length > maxLength {
					break
				}
				count := neighbors(mask, current, &adjacent)
				if count != 2 {
					if count > 2 {
						remove = append(remove, chain...)
					}
					break
				}
				chain = append(chain, current)
				next := adjacent[0]
				if next == previous {
					next = adjacent[1]
				}
				previous, current = current, next
			}
		}
		if len(remove) == 0 {
			return removed
		}
		for _, idx := range remove {
			if mask[idx]&3 != 0 {
				mask[idx] = 0
				removed++
			}
		}
	}
}
