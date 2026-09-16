#define _CRT_SECURE_NO_WARNINGS

#include "app_data.h"

#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const VehicleTypeInfo g_vehicle_types[DVM_VEHICLE_TYPE_COUNT] = {
    { "VehicleCivilianSedan", L"Olga 24", "CivilianSedan variants" },
    { "VehicleHatchback02", L"Gunter 2", "Hatchback_02 variants" },
    { "VehicleOffroad02", L"M1025 Humvee", "Offroad_02" },
    { "VehicleOffroadHatchback", L"Ada 4x4", "OffroadHatchback variants" },
    { "VehicleSedan02", L"Sarka 120", "Sedan_02 variants" },
    { "VehicleTruck01", L"M3S Truck", "Truck_01_Covered variants" },
    { "VehicleBoat", L"Motorboat", "Boat_01 variants" },
    { "VehicleMotorbike01", L"Jana 50 (Moped)", "Motorbike_01 variants" },
    { "VehicleMotorbike02", L"Bitrak 682 (Motorbike)", "Motorbike_02 variants" }
};

static const MapInfo g_maps[DVM_MAP_COUNT] = {
    {
        "chernarusplus", L"Chernarus", L"data\\maps\\chernarusplus.png",
        L"data\\roads\\chernarusplus.roads",
        L"data\\spawns\\chernarusplus.csv", 15360.0, 2048u, 2048u,
        { 55u, 56u, 21u, 81u, 59u, 111u, 72u, 97u, 97u }
    },
    {
        "enoch", L"Livonia", L"data\\maps\\enoch.png",
        L"data\\roads\\enoch.roads",
        L"data\\spawns\\enoch.csv", 12800.0, 4096u, 4096u,
        { 40u, 39u, 21u, 40u, 30u, 50u, 17u, 35u, 35u }
    },
    {
        "sakhal", L"Sakhal", L"data\\maps\\sakhal.png",
        L"data\\roads\\sakhal.roads",
        L"data\\spawns\\sakhal.csv", 15360.0, 2048u, 2048u,
        { 27u, 60u, 0u, 35u, 47u, 25u, 97u, 70u, 70u }
    },
    {
        "namalsk", L"Namalsk", L"data\\maps\\namalsk.png",
        L"data\\roads\\namalsk.roads",
        L"data\\spawns\\namalsk.csv", 12800.0, 2048u, 2048u,
        { 125u, 125u, 8u, 125u, 125u, 12u, 34u, 0u, 0u }
    },
    {
        "deerisle", L"Deer Isle", L"data\\maps\\deerisle.png",
        L"data\\roads\\deerisle.roads",
        L"data\\spawns\\deerisle.csv", 16384.0, 2048u, 2048u,
        { 33u, 38u, 0u, 69u, 39u, 11u, 0u, 0u, 0u }
    },
    {
        "bitterroot", L"Bitterroot", L"data\\maps\\bitterroot.png",
        L"data\\roads\\bitterroot.roads",
        L"data\\spawns\\bitterroot.csv", 12288.0, 2048u, 2048u,
        { 20u, 20u, 10u, 20u, 20u, 9u, 0u, 0u, 0u }
    },
    {
        "banov", L"Banov", L"data\\maps\\banov.png",
        L"data\\roads\\banov.roads",
        L"data\\spawns\\banov.csv", 15360.0, 2048u, 2048u,
        { 29u, 34u, 14u, 32u, 31u, 52u, 47u, 0u, 0u }
    }
};

const MapInfo *dvm_map_info(int map_index)
{
    if (map_index < 0 || map_index >= DVM_MAP_COUNT) return NULL;
    return &g_maps[map_index];
}

const VehicleTypeInfo *dvm_vehicle_type_info(int type_index)
{
    if (type_index < 0 || type_index >= DVM_VEHICLE_TYPE_COUNT) return NULL;
    return &g_vehicle_types[type_index];
}

int dvm_vehicle_type_from_event(const char *event_name)
{
    int index;
    if (!event_name) return -1;
    for (index = 0; index < DVM_VEHICLE_TYPE_COUNT; ++index) {
        if (strcmp(event_name, g_vehicle_types[index].event_name) == 0) return index;
    }
    return -1;
}

static char *trim_ascii(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
    return text;
}

static BOOL append_spawn(SpawnDataset *dataset, const VehicleSpawn *spawn)
{
    VehicleSpawn *larger;
    size_t new_capacity;
    if (dataset->count == dataset->capacity) {
        new_capacity = dataset->capacity ? dataset->capacity * 2u : 256u;
        if (new_capacity < dataset->capacity ||
            new_capacity > SIZE_MAX / sizeof(*dataset->items)) return FALSE;
        if (dataset->items) {
            larger = HeapReAlloc(GetProcessHeap(), 0, dataset->items,
                                 new_capacity * sizeof(*dataset->items));
        } else {
            larger = HeapAlloc(GetProcessHeap(), 0,
                               new_capacity * sizeof(*dataset->items));
        }
        if (!larger) return FALSE;
        dataset->items = larger;
        dataset->capacity = new_capacity;
    }
    dataset->items[dataset->count++] = *spawn;
    return TRUE;
}

static BOOL coordinates_duplicate(const SpawnDataset *dataset, int type_index,
                                  double x, double z)
{
    size_t index;
    for (index = 0; index < dataset->count; ++index) {
        const VehicleSpawn *item = &dataset->items[index];
        if (item->type_index == type_index && item->x == x && item->z == z)
            return TRUE;
    }
    return FALSE;
}

BOOL dvm_load_spawn_dataset(const wchar_t *base_directory, int map_index,
                            SpawnDataset *dataset, wchar_t *error,
                            size_t error_capacity)
{
    const MapInfo *map = dvm_map_info(map_index);
    wchar_t path[DVM_PATH_CAP];
    FILE *file;
    char line[512];
    unsigned int line_number = 0;
    int type_index;
    size_t index;
    if (!map || !dataset || !base_directory) return FALSE;
    ZeroMemory(dataset, sizeof(*dataset));
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index)
        dataset->type_visible[type_index] = TRUE;
    if (!dvm_join_path(path, DVM_PATH_CAP, base_directory, map->spawn_relative_path)) {
        dvm_set_error(error, error_capacity, L"Vehicle data path is too long for %ls.",
                      map->display_name);
        return FALSE;
    }
    file = _wfopen(path, L"rb");
    if (!file) {
        dvm_set_error(error, error_capacity, L"Cannot open required vehicle data:\n%ls", path);
        return FALSE;
    }
    while (fgets(line, (int)sizeof(line), file)) {
        char *event_text;
        char *x_text;
        char *z_text;
        char *first_comma;
        char *second_comma;
        char *end;
        double x;
        double z;
        VehicleSpawn spawn;
        ++line_number;
        event_text = trim_ascii(line);
        if (*event_text == '\0' || *event_text == '#') continue;
        first_comma = strchr(event_text, ',');
        second_comma = first_comma ? strchr(first_comma + 1, ',') : NULL;
        if (!first_comma || !second_comma || strchr(second_comma + 1, ',')) goto malformed;
        *first_comma = '\0';
        *second_comma = '\0';
        event_text = trim_ascii(event_text);
        x_text = trim_ascii(first_comma + 1);
        z_text = trim_ascii(second_comma + 1);
        type_index = dvm_vehicle_type_from_event(event_text);
        if (type_index < 0) goto malformed;
        errno = 0;
        x = strtod(x_text, &end);
        if (errno != 0 || end == x_text || *trim_ascii(end) != '\0' || !isfinite(x))
            goto malformed;
        errno = 0;
        z = strtod(z_text, &end);
        if (errno != 0 || end == z_text || *trim_ascii(end) != '\0' || !isfinite(z))
            goto malformed;
        if (x < 0.0 || x > map->world_size || z < 0.0 || z > map->world_size ||
            coordinates_duplicate(dataset, type_index, x, z)) goto malformed;
        spawn.type_index = type_index;
        spawn.x = x;
        spawn.z = z;
        spawn.hidden = FALSE;
        spawn.color_state = MARKER_COLOR_NORMAL;
        if (!append_spawn(dataset, &spawn)) {
            fclose(file);
            dvm_free_spawn_dataset(dataset);
            dvm_set_error(error, error_capacity, L"Not enough memory to load %ls vehicle data.",
                          map->display_name);
            return FALSE;
        }
        ++dataset->type_counts[type_index];
        continue;

malformed:
        fclose(file);
        dvm_free_spawn_dataset(dataset);
        dvm_set_error(error, error_capacity,
                      L"Malformed vehicle data at line %u:\n%ls", line_number, path);
        return FALSE;
    }
    if (ferror(file)) {
        fclose(file);
        dvm_free_spawn_dataset(dataset);
        dvm_set_error(error, error_capacity, L"Could not finish reading vehicle data:\n%ls", path);
        return FALSE;
    }
    fclose(file);
    for (index = 0; index < DVM_VEHICLE_TYPE_COUNT; ++index) {
        if (dataset->type_counts[index] != map->expected_type_counts[index]) {
            dvm_set_error(error, error_capacity,
                          L"Unexpected %ls count for %ls: %llu (expected %llu).",
                          g_vehicle_types[index].friendly_name, map->display_name,
                          (unsigned long long)dataset->type_counts[index],
                          (unsigned long long)map->expected_type_counts[index]);
            dvm_free_spawn_dataset(dataset);
            return FALSE;
        }
    }
    if (dataset->count == 0) {
        dvm_set_error(error, error_capacity, L"Vehicle data is empty:\n%ls", path);
        dvm_free_spawn_dataset(dataset);
        return FALSE;
    }
    if (error && error_capacity) error[0] = L'\0';
    return TRUE;
}

void dvm_free_spawn_dataset(SpawnDataset *dataset)
{
    if (!dataset) return;
    if (dataset->items) HeapFree(GetProcessHeap(), 0, dataset->items);
    ZeroMemory(dataset, sizeof(*dataset));
}

size_t dvm_remaining_for_type(const SpawnDataset *dataset, int type_index)
{
    size_t remaining = 0;
    size_t index;
    if (!dataset || type_index < 0 || type_index >= DVM_VEHICLE_TYPE_COUNT) return 0;
    for (index = 0; index < dataset->count; ++index) {
        if (dataset->items[index].type_index == type_index && !dataset->items[index].hidden)
            ++remaining;
    }
    return remaining;
}

size_t dvm_remaining_total(const SpawnDataset *dataset)
{
    size_t remaining = 0;
    size_t index;
    if (!dataset) return 0;
    for (index = 0; index < dataset->count; ++index)
        if (!dataset->items[index].hidden) ++remaining;
    return remaining;
}

BOOL dvm_type_has_restorable_state(const SpawnDataset *dataset, int type_index)
{
    size_t index;
    if (!dataset || type_index < 0 || type_index >= DVM_VEHICLE_TYPE_COUNT)
        return FALSE;
    for (index = 0; index < dataset->count; ++index) {
        const VehicleSpawn *spawn = &dataset->items[index];
        if (spawn->type_index == type_index &&
            (spawn->hidden || spawn->color_state != MARKER_COLOR_NORMAL)) return TRUE;
    }
    return FALSE;
}

BOOL dvm_has_restorable_state(const SpawnDataset *dataset)
{
    size_t index;
    if (!dataset) return FALSE;
    for (index = 0; index < dataset->count; ++index) {
        const VehicleSpawn *spawn = &dataset->items[index];
        if (spawn->hidden || spawn->color_state != MARKER_COLOR_NORMAL) return TRUE;
    }
    return FALSE;
}

void dvm_restore_type(SpawnDataset *dataset, int type_index)
{
    size_t index;
    if (!dataset || type_index < 0 || type_index >= DVM_VEHICLE_TYPE_COUNT) return;
    for (index = 0; index < dataset->count; ++index) {
        if (dataset->items[index].type_index == type_index) {
            dataset->items[index].hidden = FALSE;
            dataset->items[index].color_state = MARKER_COLOR_NORMAL;
        }
    }
}

void dvm_restore_all(SpawnDataset *dataset)
{
    size_t index;
    if (!dataset) return;
    for (index = 0; index < dataset->count; ++index) {
        dataset->items[index].hidden = FALSE;
        dataset->items[index].color_state = MARKER_COLOR_NORMAL;
    }
}
