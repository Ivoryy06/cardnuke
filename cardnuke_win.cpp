#include "cardnuke_win.h"
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdio>

using namespace std;

static const size_t SECTOR_SIZE = 512;

string timestamp() {
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm* tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return string(buf);
}

void log(const string& msg) {
    cout << "[" << timestamp() << "] " << msg << endl;
}

HANDLE open_device(const char* dev, BOOL write) {
    string path;
    if (strncmp(dev, "\\\\.\\PhysicalDrive", 17) == 0) {
        path = dev;
    } else if (isalpha(dev[0]) && strlen(dev) == 1) {
        path = string("\\\\.\\") + dev[0] + ":";
    } else if (isalpha(dev[0]) && dev[1] == ':') {
        path = string("\\\\.\\") + dev[0] + ":";
    } else if (isdigit(dev[0])) {
        path = "\\\\.\\PhysicalDrive" + string(dev);
    } else {
        path = dev;
    }

    HANDLE h = CreateFileA(path.c_str(),
        write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        cerr << "Error: Cannot open " << path << " - " << GetLastError() << endl;
    }
    return h;
}

BOOL close_device(HANDLE h) {
    return CloseHandle(h);
}

uint64_t get_device_size(HANDLE h) {
    GET_LENGTH_INFORMATION lenInfo;
    DWORD bytesReturned;

    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFORMATION, NULL, 0,
        &lenInfo, sizeof(lenInfo), &bytesReturned, NULL)) {
        return lenInfo.Length.QuadPart;
    }
    return 0;
}

BOOL read_sectors(HANDLE h, uint64_t start, void* buf, size_t count) {
    LARGE_INTEGER offset;
    offset.QuadPart = start * SECTOR_SIZE;

    SetFilePointer(h, offset.LowPart, &offset.HighPart, FILE_BEGIN);

    DWORD read;
    return ReadFile(h, buf, count * SECTOR_SIZE, &read, NULL) && read == count * SECTOR_SIZE;
}

BOOL write_sectors(HANDLE h, uint64_t start, const void* buf, size_t count) {
    LARGE_INTEGER offset;
    offset.QuadPart = start * SECTOR_SIZE;

    SetFilePointer(h, offset.LowPart, &offset.HighPart, FILE_BEGIN);

    DWORD written;
    return WriteFile(h, buf, count * SECTOR_SIZE, &written, NULL) && written == count * SECTOR_SIZE;
}

BOOL list_drives_windows() {
    cout << "\nDEVICE       SIZE       MODEL                          TRAN     REMOVABLE\n";
    cout << "---------------------------------------------------------------------\n";

    char letter;
    for (letter = 'A'; letter <= 'Z'; letter++) {
        string drive = string(1, letter) + ":";
        UINT type = GetDriveTypeA(drive.c_str());

        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            string volPath = "\\\\.\\" + string(1, letter) + ":";
            HANDLE h = CreateFileA(volPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, NULL);

            if (h != INVALID_HANDLE_VALUE) {
                DWORD bytes;
                STORAGE_DEVICE_NUMBER devNum;
                if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                    &devNum, sizeof(devNum), &bytes, NULL)) {

                    string model = "Unknown";
                    ULONGLONG size = 0;

                    STORAGE_PROPERTY_QUERY query = {PropertyStandardQuery, 0, sizeof(STORAGE_DESCRIPTOR_HEADER)};
                    STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)new char[1024];
                    desc->Size = 1024;
                    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        desc, 1024, &bytes, NULL);

                    if (ok && desc->VendorIdOffset) {
                        model = (char*)desc + desc->VendorIdOffset;
                    }
                    if (ok && desc->Size > 0) {
                        size = get_device_size(h);
                    }

                    double gb = size / 1024.0 / 1024.0 / 1024.0;
                    string tran = (type == DRIVE_REMOVABLE) ? "USB" : "SATA";

                    cout << letter << ":  " << fixed << setprecision(1) << setw(6) << gb << "G   "
                        << setw(-30) << model << " " << tran << "   "
                        << (type == DRIVE_REMOVABLE ? "Yes" : "No") << "\n";

                    delete[] (char*)desc;
                }
                CloseHandle(h);
            }
        }
    }

    return TRUE;
}

string get_drive_label(char letter) {
    char label[256] = {0};
    string drive = string(1, letter) + ":\\";
    if (GetVolumeInformationA(drive.c_str(), label, sizeof(label), NULL, NULL, NULL, NULL, 0)) {
        return label;
    }
    return "";
}

bool is_admin() {
    BOOL isAdmin = FALSE;
    PSID pSid = NULL;
    HANDLE hToken = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;

    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, NULL, &len);
    if (len > 0) {
        PTOKEN_USER pTokenInfo = (PTOKEN_USER)new char[len];
        if (GetTokenInformation(hToken, TokenUser, pTokenInfo, len, &len)) {
            SID_IDENTIFIER_AUTHORITY sia = SECURITY_NT_AUTHORITY;
            if (AllocateAndInitializeSid(&sia, 2, SECURITY_BUILTIN_DOMAIN_RID,
                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pSid)) {
                if (CheckTokenMembership(hToken, pSid, &isAdmin)) {
                }
                FreeSid(pSid);
            }
        }
        delete[] (char*)pTokenInfo;
    }
    CloseHandle(hToken);
    return !!isAdmin;
}

void format_drive(const string& drive, const string& fs, const string& label) {
    log("Formatting " + drive + ": with " + fs + "...");

    string volPath = "\\\\.\\" + drive;
    HANDLE h = CreateFileA(volPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        log("Error: Cannot open drive");
        return;
    }

    DELETE_USN_JOURNAL_DATA usnData = {0};
    DeviceIoControl(h, FSCTL_DELETE_USN_JOURNAL, &usnData, sizeof(usnData), NULL, 0, NULL, NULL);
    CloseHandle(h);

    string cmd = "format " + drive + " /FS:" + fs + " /V:" + label + " /Q /Y";
    log("Running: " + cmd);
    system(cmd.c_str());

    log("Done.");
}

void backup_drive(const string& drive, const string& outfile) {
    log("Backing up " + drive + ": to " + outfile + "...");

    string volPath = "\\\\.\\" + drive;
    HANDLE h = open_device(volPath.c_str(), FALSE);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    uint64_t size = get_device_size(h);
    size_t sectors = size / SECTOR_SIZE;

    const size_t CHUNK = 8 * 1024 * 1024 / SECTOR_SIZE;
    char* buf = (char*)VirtualAlloc(NULL, CHUNK * SECTOR_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!buf) {
        log("Error: Cannot allocate buffer");
        CloseHandle(h);
        return;
    }

    FILE* out = fopen(outfile.c_str(), "wb");
    if (!out) {
        log("Error: Cannot create output file");
        CloseHandle(h);
        VirtualFree(buf, 0, MEM_RELEASE);
        return;
    }

    size_t written = 0;
    double startTime = (double)GetTickCount();

    while (written < sectors) {
        size_t todo = min(CHUNK, sectors - written);
        if (!read_sectors(h, written, buf, todo)) {
            log("Error: Read failed at sector " + to_string(written));
            break;
        }
        fwrite(buf, SECTOR_SIZE, todo, out);
        written += todo;

        double elapsed = (GetTickCount() - startTime) / 1000.0;
        double mb = written * SECTOR_SIZE / 1024.0 / 1024.0;
        double speed = elapsed > 0 ? mb / elapsed : 0;
        cout << "\r  Written: " << fixed << setprecision(1) << mb << " MB  Speed: " << speed << " MB/s   " << flush;
    }

    fclose(out);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);

    log("Done.");
}

void restore_drive(const string& infile, const string& drive) {
    log("Restoring " + infile + " to " + drive + ":...");

    FILE* in = fopen(infile.c_str(), "rb");
    if (!in) {
        log("Error: Cannot open input file");
        return;
    }

    string volPath = "\\\\.\\" + drive;
    HANDLE h = open_device(volPath.c_str(), TRUE);
    if (h == INVALID_HANDLE_VALUE) {
        fclose(in);
        return;
    }

    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);

    size_t sectors = size / SECTOR_SIZE;
    const size_t CHUNK = 8 * 1024 * 1024 / SECTOR_SIZE;
    char* buf = (char*)VirtualAlloc(NULL, CHUNK * SECTOR_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!buf) {
        log("Error: Cannot allocate buffer");
        fclose(in);
        CloseHandle(h);
        return;
    }

    size_t written = 0;
    double startTime = (double)GetTickCount();

    while (written < sectors) {
        size_t todo = min(CHUNK, sectors - written);
        fread(buf, SECTOR_SIZE, todo, in);
        if (!write_sectors(h, written, buf, todo)) {
            log("Error: Write failed at sector " + to_string(written));
            break;
        }
        written += todo;

        double elapsed = (GetTickCount() - startTime) / 1000.0;
        double mb = written * SECTOR_SIZE / 1024.0 / 1024.0;
        double speed = elapsed > 0 ? mb / elapsed : 0;
        cout << "\r  Written: " << fixed << setprecision(1) << mb << " MB  Speed: " << speed << " MB/s   " << flush;
    }

    FlushFileBuffers(h);
    fclose(in);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);

    log("Done.");
}

void speed_test_drive(const string& drive) {
    log("Speed test for " + drive + ":...");

    string volPath = "\\\\.\\" + drive;
    HANDLE h = open_device(volPath.c_str(), TRUE);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    uint64_t size = get_device_size(h);
    const size_t TEST_SIZE = 256 * 1024 * 1024;
    const size_t CHUNK = 8 * 1024 * 1024;

    char* buf = (char*)VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        log("Error: Cannot allocate buffer");
        CloseHandle(h);
        return;
    }

    for (size_t i = 0; i < CHUNK; i++) {
        buf[i] = (i * 31) & 0xFF;
    }

    double writeSpeed = 0, readSpeed = 0;
    DWORD startTime = GetTickCount();

    size_t todo = min(TEST_SIZE / CHUNK, size / SECTOR_SIZE);
    for (size_t i = 0; i < todo; i++) {
        write_sectors(h, i * (CHUNK / SECTOR_SIZE), buf, CHUNK / SECTOR_SIZE);
    }
    FlushFileBuffers(h);
    double writeTime = (GetTickCount() - startTime) / 1000.0;
    writeSpeed = TEST_SIZE / writeTime / 1024.0 / 1024.0;

    startTime = GetTickCount();
    for (size_t i = 0; i < todo; i++) {
        read_sectors(h, i * (CHUNK / SECTOR_SIZE), buf, CHUNK / SECTOR_SIZE);
    }
    double readTime = (GetTickCount() - startTime) / 1000.0;
    readSpeed = TEST_SIZE / readTime / 1024.0 / 1024.0;

    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);

    ostringstream ss;
    ss << "  Write: " << fixed << setprecision(1) << writeSpeed << " MB/s  Read: " << readSpeed << " MB/s";
    log(ss.str());
}

void eject_drive(const string& drive) {
    log("Ejecting " + drive + ":...");

    string cmd = "powershell -Command \"(Get-WmiObject -Class Win32_PhysicalDisk | "
               "Where-Object {$_.DeviceID -match '" + drive + "'}).Eject()\"";
    system(cmd.c_str());

    log("Done.");
}

void print_usage(const char* prog) {
    cout << "=== cardnuke (Windows) ===\n\n";
    cout << "Usage: " << prog << " <drive> <mode> [options]\n\n";
    cout << "Drive:\n";
    cout << "  Letter (e.g., D) or number (0 for PhysicalDrive0)\n\n";
    cout << "Modes:\n";
    cout << "  1 format  - wipe/reformat (NTFS/FAT32/exFAT)\n";
    cout << "  2 backup  - image the drive to a .img file\n";
    cout << "  3 restore - flash a .img backup back to drive\n";
    cout << "  4 speed   - benchmark read/write speed\n";
    cout << "  5 info    - show drive info\n";
    cout << "  6 eject   - safely remove drive\n";
    cout << "\nExamples:\n";
    cout << "  " << prog << " D 1        # format drive D:\n";
    cout << "  " << prog << " D 2 backup.img  # backup to file\n";
    cout << "  " << prog << " 0 4          # speed test PhysicalDrive0\n";
}

int main(int argc, char* argv[]) {
    cout << "=== cardnuke (Windows) ===\n";

    if (!is_admin()) {
        cout << "Warning: Run as Administrator for full functionality\n";
    }

    if (argc < 3) {
        list_drives_windows();
        print_usage(argv[0]);
        return 1;
    }

    string drive = argv[1];
    string mode = argv[2];

    log("=== cardnuke started ===");

    if (mode == "1" || mode == "format") {
        string fs = argc > 3 ? argv[3] : "NTFS";
        string label = argc > 4 ? argv[4] : "CARD";
        format_drive(drive, fs, label);
    } else if (mode == "2" || mode == "backup") {
        string outfile = argc > 3 ? argv[3] : "backup.img";
        backup_drive(drive, outfile);
    } else if (mode == "3" || mode == "restore") {
        string infile = argc > 3 ? argv[3] : "backup.img";
        restore_drive(infile, drive);
    } else if (mode == "4" || mode == "speed") {
        speed_test_drive(drive);
    } else if (mode == "5" || mode == "info") {
        list_drives_windows();
    } else if (mode == "6" || mode == "eject") {
        eject_drive(drive);
    } else if (mode == "list" || mode == "--list") {
        list_drives_windows();
    } else {
        cerr << "Unknown mode: " << mode << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}