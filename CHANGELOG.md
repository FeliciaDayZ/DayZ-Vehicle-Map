# Patch notes

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

### Testing

- Added automated frame-pacing coverage for rapid pan/zoom input, resizing,
  idle behavior, minimize/restore, and hidden-window behavior.
- Updated the GUI smoke test to account for capped frame delivery.
