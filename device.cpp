#include "cardnuke.h"
#include <iostream>
#include <vector>
#include <map>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>
#include <mntent.h>
#endif

bool is_admin() {
#ifdef _WIN32
    BOOL isAdmin;
    PSID pSid;
    CHECK_TOKEN_INFORMATION pTokenInfo = nullptr;
    HANDLE hToken = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, nullptr);
    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (len > 0) {
        pTokenInfo = (CHECK_TOKEN_INFORMATION) new char[len];
        if (GetTokenInformation(hToken, TokenUser, pTokenInfo, len, &len)) {
            pSid = pTokenInfo->User.Sid;
            isAdmin = CheckTokenMembership(pSid, SECURITY_BUILTIN_DOMAIN_RID);
        }
    }
    CloseHandle(hToken);
    return !!isAdmin;
#else
    return geteuid() == 0;
#endif
}

std::string normalize_device(const std::string& dev) {
    if (dev.empty()) return "";

    std::string d = dev;
    if (d.find("/dev/") == 0) return d;

#ifdef _WIN32
    if (isdigit(d[0])) return "\\\\.\\PhysicalDrive" + d;
    if (d.size() == 1 && isalpha(d[0])) {
        d = d[0] + ":";
    }
#endif

    return d;
}

#ifndef _WIN32
void list_devices_linux() {
    std::vector<std::pair<std::string, std::string>> devs;

    // Check /sys/block for block devices
    DIR* dir = opendir("/sys/block");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir))) {
            std::string name = ent->d_name;
            if (name.substr(0, 2) == "sd" || name.substr(0, 4) == "nvme" || name.substr(0, 6) == "mmcblk") {
                std::string path = "/dev/" + name;
                struct stat st;
                if (stat(path.c_str(), &st) == 0) {
                    // Get size
                    std::string size_path = "/sys/block/" + name + "/size";
                    std::ifstream sz(size_path);
                    long long sectors = 0;
                    if (sz) sz >> sectors;
                    long long bytes = sectors * 512;

                    // Get model/vendor
                    std::string model_path = "/sys/block/" + name + "/device/model";
                    std::string model;
                    std::ifstream mf(model_path);
                    if (mf) {
                        std::getline(mf, model);
                        // Trim whitespace
                        size_t start = model.find_first_not_of(" \t\n\r");
                        size_t end = model.find_last_not_of(" \t\n\r");
                        if (start != std::string::npos) {
                            model = model.substr(start, end - start + 1);
                        } else {
                            model = "Unknown";
                        }
                    }

                    // Get transport
                    std::string tran;
                    std::string tran_path = "/sys/block/" + name + "/device/transport";
                    std::ifstream tf(tran_path);
                    if (tf) std::getline(tf, tran);

                    std::ostringstream ss;
                    ss << name << " " << (bytes / 1024 / 1024 / 1024) << "G " << model << " " << tran;
                    devs.push_back({name, ss.str()});
                }
            }
        }
        closedir(dir);
    }

    // Print header
    std::cout << "\nDEVICE       SIZE       MODEL                          TRAN     REMOVABLE\n";
    std::cout << "--------------------------------------------------------------------------------------\n";

    for (auto& d : devs) {
        std::cout << d.second << "\n";
    }
}

void list_devices_windows() {
    std::cout << "\nDEVICE       SIZE       MODEL                          TRAN     REMOVABLE\n";
    std::cout << "--------------------------------------------------------------------------------------\n";

    // Use WMI to enumerate disks
    system("wmic diskdrive get model,size,interfacetype,status /format:list");
}
#endif

void list_devices() {
#ifdef _WIN32
    list_devices_windows();
#else
    list_devices_linux();
#endif
}

std::string get_filesystem(const std::string& dev) {
#ifndef _WIN32
    std::string part = dev;
    if (dev.find("/dev/") == 0) {
        // Get mount point
        FILE* mnt = setmntent("/proc/mounts", "r");
        if (mnt) {
            struct mntent* m;
            while ((m = getmntent(mnt))) {
                if (std::string(m->mnt_fsname) == dev) {
                    endmntent(mnt);
                    return m->mnt_type;
                }
            }
            endmntent(mnt);
        }
    }
#endif
    return "";
}

bool mount_device(const std::string& dev, const std::string& mnt) {
#ifdef _WIN32
    return false;  // Windows uses drive letters
#else
    std::string cmd = "mount -o sync " + dev + " " + mnt;
    return system(cmd.c_str()) == 0;
#endif
}

bool unmount_device(const std::string& mnt) {
#ifdef _WIN32
    return false;
#else
    std::string cmd = "umount " + mnt;
    return system(cmd.c_str()) == 0;
#endif
}

bool eject_device(const std::string& dev) {
#ifdef _WIN32
    std::string cmd = "powershell -Command \"(Get-WmiObject -Class Win32_PhysicalDisk | Where-Object {$_.DeviceID -match '" + dev + "'}).Eject()\"";
    return system(cmd.c_str()) == 0;
#else
    std::string cmd = "eject " + dev;
    return system(cmd.c_str()) == 0;
#endif
}