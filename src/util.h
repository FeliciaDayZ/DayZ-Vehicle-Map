#ifndef DAYZ_VEHICLE_MAP_UTIL_H
#define DAYZ_VEHICLE_MAP_UTIL_H

#include <stddef.h>
#include <windows.h>

#define DVM_PATH_CAP 1024

BOOL dvm_executable_directory(wchar_t *directory, size_t capacity);
BOOL dvm_join_path(wchar_t *destination, size_t capacity,
                   const wchar_t *directory, const wchar_t *relative);
void dvm_set_error(wchar_t *error, size_t capacity, const wchar_t *format, ...);

#endif
