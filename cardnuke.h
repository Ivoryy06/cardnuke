#ifndef CARDNUKE_H
#define CARDNUKE_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mntent.h>
#include <sys/vfs.h>
#endif

std::string logfile;
bool is_windows();
void log(const std::string& msg, const std::string& fn = "");
std::string timestamp();
void list_devices();
std::string normalize_device(const std::string& dev);
std::string get_filesystem(const std::string& dev);
bool mount_device(const std::string& dev, const std::string& mnt);
bool unmount_device(const std::string& mnt);
std::string get_mount_point(const std::string& dev);
bool eject_device(const std::string& dev);
bool is_admin();

#endif