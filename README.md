# DayZ Vehicle Map

DayZ Vehicle Map is a portable native Windows application for viewing possible
vehicle spawn locations across several DayZ maps. It runs completely offline
and combines satellite map images, vector road overlays, and map-specific
vehicle spawn data in one responsive desktop interface.

## Supported maps

- Chernarus
- Livonia
- Sakhal
- Namalsk
- Deer Isle
- Bitterroot
- Banov

## Features

- Filter markers by vehicle type, with multiple types visible at once.
- Left-click an individual marker to check it off and hide it.
- Right-click a marker to toggle its state between yellow and green.
- Middle-click a marker to toggle its state between yellow and red.
- Restore all markers on the current map or restore one vehicle type.
- Zoom with the mouse wheel and drag the map to pan.
- Preserve checked, green, and red marker states between runs.
- Keep marker state isolated by map, vehicle type, and spawn coordinates.
- Render paved and dirt roads as scalable vector lines while zooming.
- Use no Internet connection, registry settings, or external runtime.

## Using the program

Keep `DayZVehicleMap.exe` beside the `data` directory, then run the executable.

- Select a map from the map dropdown.
- Enable or disable vehicle types with the filter controls.
- Use the mouse wheel to zoom around the cursor.
- Hold and drag the left mouse button to pan.
- Left-click a visible marker to check and hide that spawn point.
- Right-click a marker to mark it green.
- Middle-click a marker to mark it red.
- Use Restore All or the per-type Restore controls to reset markers.

The application writes personal marker state to `checked_state.txt` beside the
executable. That file is optional and is created automatically when needed.

## Vehicle spawn counts

| Map | Spawn markers |
|---|---:|
| Chernarus | 649 |
| Livonia | 307 |
| Sakhal | 431 |
| Namalsk | 554 |
| Deer Isle | 190 |
| Bitterroot | 99 |
| Banov | 239 |

Vehicle filters include the supported cars, trucks, boats, Jana 50 mopeds, and
Bitrak 682 motorbikes present in each map's source data.

## Building

The project is written in C17 for native 64-bit Windows using the Win32 API and
GDI+. The supplied build script expects w64devkit under `C:\winprog\C\bin`.

Run:

```bat
build.bat
```

The resulting GUI executable is written to the project root as
`DayZVehicleMap.exe`.

To rebuild and run the native model, persistence, and GUI smoke tests:

```bat
test.bat
```

## Runtime layout

The application resolves all runtime files relative to the executable rather
than the current working directory:

```text
DayZVehicleMap.exe
data/
  marker.png
  maps/
  roads/
  spawns/
```

The files under `data/source` and the programs under `tools` are retained for
data provenance and regeneration; they are not required during normal use.

## Data sources

- Vanilla Chernarus, Livonia, and Sakhal vehicle positions come from DayZ
  Server Experimental mission data.
- Namalsk, Deer Isle, Bitterroot, and Banov use their respective mission data.
- Satellite map imagery was assembled from iZurvive map tiles.
- Chernarus and Livonia road geometry was derived from Survivorm route data.
- Community-map road geometry was exported from the corresponding terrain WRP
  data; Sakhal roads were traced from high-resolution XAM topographic tiles.

Runtime CSV and road files are included, so DayZ Server and workshop maps do
not need to remain installed.
