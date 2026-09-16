#define _CRT_SECURE_NO_WARNINGS

#include "state.h"

#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define STATE_HEADER_V1 "DAYZVEHICLEMAP_STATE_V1"
#define STATE_HEADER_V2 "DAYZVEHICLEMAP_STATE_V2"

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

static int map_from_id(const char *id)
{
    int index;
    for (index = 0; index < DVM_MAP_COUNT; ++index) {
        const MapInfo *map = dvm_map_info(index);
        if (map && strcmp(map->id, id) == 0) return index;
    }
    return -1;
}

static VehicleSpawn *find_matching_spawn(SpawnDataset *dataset, int type_index,
                                         double x, double z)
{
    size_t index;
    for (index = 0; index < dataset->count; ++index) {
        VehicleSpawn *spawn = &dataset->items[index];
        if (spawn->type_index == type_index &&
            fabs(spawn->x - x) <= 0.0000001 &&
            fabs(spawn->z - z) <= 0.0000001) {
            return spawn;
        }
    }
    return NULL;
}

static int split_fields(char *text, char **fields, int capacity)
{
    int count = 0;
    while (text && count < capacity) {
        char *separator;
        fields[count++] = trim_ascii(text);
        separator = strchr(text, ',');
        if (!separator) return count;
        if (count == capacity) return capacity + 1;
        *separator = '\0';
        fields[count - 1] = trim_ascii(fields[count - 1]);
        text = separator + 1;
    }
    return count;
}

static BOOL parse_number(const char *text, double *value)
{
    char *end;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *trim_ascii(end) == '\0' && isfinite(*value);
}

static BOOL parse_v2_state(const char *hidden_text, const char *color_text,
                           BOOL *hidden, MarkerColorState *color_state)
{
    if (strcmp(hidden_text, "0") == 0) *hidden = FALSE;
    else if (strcmp(hidden_text, "1") == 0) *hidden = TRUE;
    else return FALSE;
    if (strcmp(color_text, "normal") == 0) *color_state = MARKER_COLOR_NORMAL;
    else if (strcmp(color_text, "green") == 0) *color_state = MARKER_COLOR_GREEN;
    else if (strcmp(color_text, "red") == 0) *color_state = MARKER_COLOR_RED;
    else return FALSE;
    return TRUE;
}

static const char *color_state_text(MarkerColorState color_state)
{
    if (color_state == MARKER_COLOR_GREEN) return "green";
    if (color_state == MARKER_COLOR_RED) return "red";
    return "normal";
}

static void reset_loaded_state(SpawnDataset maps[DVM_MAP_COUNT])
{
    int map_index;
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        size_t index;
        for (index = 0; index < maps[map_index].count; ++index) {
            maps[map_index].items[index].hidden = FALSE;
            maps[map_index].items[index].color_state = MARKER_COLOR_NORMAL;
        }
    }
}

static BOOL replace_state_file_with_retry(const wchar_t *temporary,
                                          const wchar_t *path)
{
    int attempt;
    for (attempt = 0; attempt < 40; ++attempt) {
        DWORD error;
        if (MoveFileExW(temporary, path,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return TRUE;
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION &&
            error != ERROR_ACCESS_DENIED) return FALSE;
        Sleep(25);
    }
    return FALSE;
}

BOOL dvm_state_load(const wchar_t *path, SpawnDataset maps[DVM_MAP_COUNT],
                    wchar_t *warning, size_t warning_capacity)
{
    FILE *file;
    char line[512];
    unsigned int line_number = 0;
    unsigned int ignored = 0;
    int state_version = 0;
    BOOL saw_header = FALSE;
    int map_index;
    if (warning && warning_capacity) warning[0] = L'\0';
    if (!path || !maps) return FALSE;
    reset_loaded_state(maps);
    file = _wfopen(path, L"rb");
    if (!file) {
        if (errno == ENOENT) return TRUE;
        dvm_set_error(warning, warning_capacity, L"Could not read state file; all markers were restored:\n%ls", path);
        return FALSE;
    }
    while (fgets(line, (int)sizeof(line), file)) {
        char *text = trim_ascii(line);
        char *fields[6];
        int field_count;
        int type_index;
        double x;
        double z;
        BOOL hidden = TRUE;
        MarkerColorState color_state = MARKER_COLOR_NORMAL;
        VehicleSpawn *spawn;
        ++line_number;
        if (*text == '\0' || *text == '#') continue;
        if (!saw_header) {
            saw_header = TRUE;
            if (strcmp(text, STATE_HEADER_V1) == 0) state_version = 1;
            else if (strcmp(text, STATE_HEADER_V2) == 0) state_version = 2;
            else ++ignored;
            continue;
        }
        if (state_version == 0) { ++ignored; continue; }
        field_count = split_fields(text, fields, 6);
        if ((state_version == 1 && field_count != 4) ||
            (state_version == 2 && field_count != 6)) { ++ignored; continue; }
        map_index = map_from_id(fields[0]);
        type_index = dvm_vehicle_type_from_event(fields[1]);
        if (map_index < 0 || type_index < 0) { ++ignored; continue; }
        if (!parse_number(fields[2], &x) || !parse_number(fields[3], &z) ||
            (state_version == 2 &&
             !parse_v2_state(fields[4], fields[5], &hidden, &color_state))) {
            ++ignored;
            continue;
        }
        spawn = find_matching_spawn(&maps[map_index], type_index, x, z);
        if (!spawn) { ++ignored; continue; }
        spawn->hidden = hidden;
        spawn->color_state = color_state;
    }
    if (ferror(file)) ++ignored;
    fclose(file);
    if (line_number == 0) return TRUE;
    if (!saw_header || ignored != 0) {
        dvm_set_error(warning, warning_capacity,
                      L"The state file contained %u unrecognized or stale line(s). "
                      L"They were safely ignored.", ignored + (saw_header ? 0u : 1u));
    }
    return TRUE;
}

BOOL dvm_state_save(const wchar_t *path, const SpawnDataset maps[DVM_MAP_COUNT],
                    wchar_t *error, size_t error_capacity)
{
    wchar_t temporary[DVM_PATH_CAP];
    FILE *file;
    int map_index;
    int written;
    if (!path || !maps) return FALSE;
    written = _snwprintf(temporary, DVM_PATH_CAP, L"%ls.tmp", path);
    if (written < 0 || written >= DVM_PATH_CAP) {
        dvm_set_error(error, error_capacity, L"State path is too long.");
        return FALSE;
    }
    file = _wfopen(temporary, L"wb");
    if (!file) {
        dvm_set_error(error, error_capacity, L"Cannot write state file:\n%ls", temporary);
        return FALSE;
    }
    if (fprintf(file, "%s\n# map,event,x,z,hidden,color\n", STATE_HEADER_V2) < 0)
        goto write_failed;
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        const MapInfo *map = dvm_map_info(map_index);
        const SpawnDataset *dataset = &maps[map_index];
        size_t index;
        for (index = 0; index < dataset->count; ++index) {
            const VehicleSpawn *spawn = &dataset->items[index];
            const VehicleTypeInfo *type;
            if (!spawn->hidden && spawn->color_state == MARKER_COLOR_NORMAL) continue;
            type = dvm_vehicle_type_info(spawn->type_index);
            if (!map || !type || fprintf(file, "%s,%s,%.10f,%.10f,%d,%s\n",
                                         map->id, type->event_name,
                                         spawn->x, spawn->z,
                                         spawn->hidden ? 1 : 0,
                                         color_state_text(spawn->color_state)) < 0)
                goto write_failed;
        }
    }
    if (fflush(file) != 0 || fclose(file) != 0) {
        file = NULL;
        goto replace_failed;
    }
    file = NULL;
    if (!replace_state_file_with_retry(temporary, path))
        goto replace_failed;
    if (error && error_capacity) error[0] = L'\0';
    return TRUE;

write_failed:
    fclose(file);
    file = NULL;
replace_failed:
    if (file) fclose(file);
    DeleteFileW(temporary);
    dvm_set_error(error, error_capacity, L"Could not save marker state:\n%ls", path);
    return FALSE;
}
