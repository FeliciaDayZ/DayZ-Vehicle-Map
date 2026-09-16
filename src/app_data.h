#ifndef DAYZ_VEHICLE_MAP_APP_DATA_H
#define DAYZ_VEHICLE_MAP_APP_DATA_H

#include <stddef.h>
#include <windows.h>

#define DVM_MAP_COUNT 7
#define DVM_VEHICLE_TYPE_COUNT 9

typedef enum MarkerColorState {
    MARKER_COLOR_NORMAL = 0,
    MARKER_COLOR_GREEN = 1,
    MARKER_COLOR_RED = 2
} MarkerColorState;

typedef struct VehicleTypeInfo {
    const char *event_name;
    const wchar_t *friendly_name;
    const char *class_family;
} VehicleTypeInfo;

typedef struct VehicleSpawn {
    int type_index;
    double x;
    double z;
    BOOL hidden;
    MarkerColorState color_state;
} VehicleSpawn;

typedef struct SpawnDataset {
    VehicleSpawn *items;
    size_t count;
    size_t capacity;
    size_t type_counts[DVM_VEHICLE_TYPE_COUNT];
    BOOL type_visible[DVM_VEHICLE_TYPE_COUNT];
} SpawnDataset;

typedef struct MapInfo {
    const char *id;
    const wchar_t *display_name;
    const wchar_t *image_relative_path;
    const wchar_t *road_relative_path;
    const wchar_t *spawn_relative_path;
    double world_size;
    UINT expected_image_width;
    UINT expected_image_height;
    size_t expected_type_counts[DVM_VEHICLE_TYPE_COUNT];
} MapInfo;

const MapInfo *dvm_map_info(int map_index);
const VehicleTypeInfo *dvm_vehicle_type_info(int type_index);
int dvm_vehicle_type_from_event(const char *event_name);
BOOL dvm_load_spawn_dataset(const wchar_t *base_directory, int map_index,
                            SpawnDataset *dataset, wchar_t *error,
                            size_t error_capacity);
void dvm_free_spawn_dataset(SpawnDataset *dataset);
size_t dvm_remaining_for_type(const SpawnDataset *dataset, int type_index);
size_t dvm_remaining_total(const SpawnDataset *dataset);
BOOL dvm_type_has_restorable_state(const SpawnDataset *dataset, int type_index);
BOOL dvm_has_restorable_state(const SpawnDataset *dataset);
void dvm_restore_type(SpawnDataset *dataset, int type_index);
void dvm_restore_all(SpawnDataset *dataset);

#endif
