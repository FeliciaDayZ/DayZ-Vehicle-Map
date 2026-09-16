#define _CRT_SECURE_NO_WARNINGS

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

BOOL dvm_executable_directory(wchar_t *directory, size_t capacity)
{
    DWORD length;
    wchar_t *slash;
    if (!directory || capacity < 2 || capacity > (size_t)MAXDWORD) return FALSE;
    length = GetModuleFileNameW(NULL, directory, (DWORD)capacity);
    if (length == 0 || (size_t)length >= capacity) return FALSE;
    slash = wcsrchr(directory, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    return TRUE;
}

BOOL dvm_join_path(wchar_t *destination, size_t capacity,
                   const wchar_t *directory, const wchar_t *relative)
{
    int written;
    if (!destination || !directory || !relative || capacity == 0) return FALSE;
    written = _snwprintf(destination, capacity, L"%ls\\%ls", directory, relative);
    if (written < 0 || (size_t)written >= capacity) {
        destination[capacity - 1] = L'\0';
        return FALSE;
    }
    return TRUE;
}

void dvm_set_error(wchar_t *error, size_t capacity, const wchar_t *format, ...)
{
    va_list arguments;
    if (!error || capacity == 0) return;
    va_start(arguments, format);
    _vsnwprintf(error, capacity, format, arguments);
    va_end(arguments);
    error[capacity - 1] = L'\0';
}
