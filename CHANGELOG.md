# Patch notes

## v1.1.0 — 2026-10-17

### Performance

- Raised the map rendering cap from 25 FPS to 35 FPS.
- Cached rendered Nasdara structures by area and zoom, with nearby tile prefetch
  and reuse of existing detail during zoom changes.
- Cached decoded model pictures in memory to avoid repeated image decoding.
- Moved structure rendering and picture decoding to a background worker, which
  prioritizes the current view and sleeps when no work remains.
- Added a spatial index to avoid scanning all 295,195 objects on each redraw.
- Removed slow repeated image resampling from detailed frames while retaining
  the FPS limit, idle behavior, and bounded memory caches.
- In a 1920 x 1080 Nasdara benchmark, average drag rendering fell from about
  73 ms to 6 ms and detailed redraws from about 268 ms to 17 ms. These measure
  rendering time; actual frame rates still depend on the FPS cap and hardware.

### Maps

- Added Nasdara's buildings, tents, containers, and other above-ground structures
  with model footprints, top-down pictures, placement angles, and instance scales.
- Replaced Nasdara's traced road data with terrain road geometry from DayZed.GG,
  removing building and wall pixels previously mistaken for dirt roads.
- Corrected Nasdara's paved and dirt road tracing to remove small loops and
  stray branches along otherwise straight or curved roads.
- Regenerated the Nasdara road overlay; all other maps retain their existing
  road data.

### Validation and distribution

- Expanded regression checks for structure position, rotation, scale, picture
  orientation, tile reuse, tile seams, background rendering, and cache eviction.
- Verified the 35 FPS cap, idle behavior, map switching, and saved marker
  persistence with the native test suite.
- Rebuilt the Windows executable as version 1.1.0 and included a portable ZIP
  with the required runtime map data. Existing saved marker states are preserved.

## 2026-10-16

### Maps

- Added Nasdara from DayZ: Badlands (`dayzOffline.nasdara`) with a calibrated
  4096 x 4096 satellite map and paved/dirt road overlays.
- Added 435 vehicle spawn markers, including Jana 50 and Bitrak 682 locations,
  with independent filters and saved marker state.

## 2026-09-26

### Performance

- Capped map rendering at a maximum of 25 FPS during panning, zooming, resizing,
  and other viewport updates.
- Stopped continuous map rendering while the app is idle.
- Paused map rendering while the app is minimized or hidden, then rendered any
  pending changes after it became visible again.
- Combined rapid input into the next scheduled frame and reused the previous
  frame for unchanged Windows repaint requests to reduce CPU contention while
  playing DayZ.
