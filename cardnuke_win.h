#ifndef CARDNUKE_WIN_H
#define CARDNUKE_WIN_H

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

HANDLE open_device(const char* dev, BOOL write);
BOOL close_device(HANDLE h);
BOOL read_sectors(HANDLE h, uint64_t start, void* buf, size_t count);
BOOL write_sectors(HANDLE h, uint64_t start, const void* buf, size_t count);
uint64_t get_device_size(HANDLE h);
BOOL list_drives_windows();
std::string get_drive_label(char letter);

#ifdef __cplusplus
}
#endif

#endif