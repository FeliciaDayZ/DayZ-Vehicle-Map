# Patch notes

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
