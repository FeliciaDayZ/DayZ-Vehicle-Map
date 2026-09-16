#ifndef DAYZ_VEHICLE_MAP_STATE_H
#define DAYZ_VEHICLE_MAP_STATE_H

#include "app_data.h"

#include <stddef.h>
#include <windows.h>

BOOL dvm_state_load(const wchar_t *path, SpawnDataset maps[DVM_MAP_COUNT],
                    wchar_t *warning, size_t warning_capacity);
BOOL dvm_state_save(const wchar_t *path, const SpawnDataset maps[DVM_MAP_COUNT],
                    wchar_t *error, size_t error_capacity);

#endif
