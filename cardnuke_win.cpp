#include "cardnuke_win.h"
// cardnuke (Windows): format, back up, restore, benchmark and eject drives.
// Inputs are validated before use, the Windows system drive is refused, and
// every destructive mode asks first. made by Ivoryy06 ♡
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace std;

static const size_t SECTOR_SIZE = 512;

// All user input is checked before it reaches `format` or PowerShell.
static string upper(string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static string normalize_drive(const string& d) {
    string s = d;
    if (s.size() == 2 && isalpha((unsigned char)s[0]) && s[1] == ':') s = s.substr(0, 1);
    if (s.size() == 1) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

static bool valid_drive_letter(const string& d) {
    return d.size() == 1 && isalpha((unsigned char)d[0]);
}

static bool valid_label(const string& s, size_t maxlen) {
    if (s.empty() || s.size() > maxlen) return false;
    return all_of(s.begin(), s.end(), [](unsigned char c) {
        return isalnum(c) || c == ' ' || c == '-' || c == '_';
    });
}

static bool valid_disk_target(const string& d) {
    if (valid_drive_letter(d)) return true;
    return !d.empty() && all_of(d.begin(), d.end(), [](unsigned char c) {
        return isdigit(c);
    });
}

// Refuses the disk Windows is running from (the Linux version does the same).
static bool is_system_target(const string& drive) {
    if (valid_drive_letter(drive)) {
        char win[MAX_PATH];
        if (GetWindowsDirectoryA(win, MAX_PATH)) {
            return (char)toupper((unsigned char)win[0]) == drive[0];
        }
        return false;
    }

    string id = drive;
    size_t pos = id.find("PhysicalDrive");
    if (pos != string::npos) id = id.substr(pos + 13);

    bool digits = !id.empty() && all_of(id.begin(), id.end(), [](unsigned char c) { return isdigit(c); });
    if (!digits) return false;

    HANDLE h = CreateFileA("\\\\.\\C:", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_DEVICE_NUMBER info = {};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
        &info, sizeof(info), &bytes, NULL);
    CloseHandle(h);
    return ok && to_string(info.DeviceNumber) == id;
}

int ask_yes_no(const char* prompt) {
    cout << prompt << " [y/N]: ";
    string r;
    getline(cin, r);
    return (r == "y" || r == "Y" || r == "yes");
}

string ask_choice(const char* prompt, const char* def) {
    cout << prompt << " (default " << def << "): ";
    string r;
    getline(cin, r);
    return r.empty() ? def : r;
}

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
    if (!dev || !*dev) {
        cerr << "Error: No device given." << endl;
        return INVALID_HANDLE_VALUE;
    }

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

                    STORAGE_PROPERTY_QUERY query = {};
                    query.PropertyId = StorageDeviceProperty;
                    query.QueryType = PropertyStandardQuery;
                    vector<char> descBuf(1024, 0);
                    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descBuf.data());
                    desc->Size = (DWORD)descBuf.size();
                    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        desc, (DWORD)descBuf.size(), &bytes, NULL);

                    if (ok && desc->VendorIdOffset && desc->VendorIdOffset < descBuf.size()) {
                        const char* vendor = descBuf.data() + desc->VendorIdOffset;
                        size_t avail = descBuf.size() - desc->VendorIdOffset;
                        size_t len = 0;
                        while (len < avail && vendor[len]) len++;
                        model = string(vendor, len);
                    }
                    if (ok && desc->Size > 0) {
                        size = get_device_size(h);
                    }

                    double gb = size / 1024.0 / 1024.0 / 1024.0;
                    string tran = (type == DRIVE_REMOVABLE) ? "USB" : "SATA";

                    cout << letter << ":  " << fixed << setprecision(1) << setw(6) << gb << "G   "
                        << left << setw(30) << model << tran << "   "
                        << (type == DRIVE_REMOVABLE ? "Yes" : "No") << "\n";

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

void format_drive(const string& rawDrive, const string& fs, const string& label) {
    string drive = normalize_drive(rawDrive);
    if (!valid_drive_letter(drive)) {
        log("Format needs a single drive letter, like D.");
        return;
    }

    string filesystem = upper(fs);
    if (filesystem != "NTFS" && filesystem != "FAT32" && filesystem != "EXFAT") {
        log("Filesystem must be NTFS, FAT32 or exFAT.");
        return;
    }

    size_t maxLabel = filesystem == "NTFS" ? 32 : 11;
    if (!valid_label(label, maxLabel)) {
        log("Label can use letters, numbers, spaces, - and _ (up to " + to_string(maxLabel) + " characters).");
        return;
    }
    if (is_system_target(drive)) {
        log("Refusing: " + drive + ": is the drive Windows is running from.");
        return;
    }

    log("Formatting " + drive + ": with " + filesystem + "...");

    cout << "\n⚠️  WARNING: This will ERASE ALL DATA on drive " << drive << ":!\n";
    if (!ask_yes_no("Are you sure you want to continue?")) {
        log("Cancelled.");
        return;
    }

    // No shell variables here: every part of this command was validated above.
    string cmd = "format " + drive + ": /FS:" + filesystem + " /V:\"" + label + "\" /Q /Y";
    log("Running: " + cmd);
    int rc = system(cmd.c_str());

    if (rc == 0) log("Done.");
    else log("Format exited with code " + to_string(rc) + ".");
}

void backup_drive(const string& rawDrive, const string& outfile) {
    string drive = normalize_drive(rawDrive);
    if (!valid_disk_target(drive)) {
        log("Give a drive letter like D, or a disk number like 0.");
        return;
    }
    if (outfile.empty() || outfile.find("\\\\.\\") == 0 || (outfile.size() == 2 && outfile[1] == ':')) {
        log("Choose a normal file for the backup, not a drive.");
        return;
    }

    log("Backing up " + drive + ": to " + outfile + "...");

    cout << "\nThis will backup drive " << drive << ": to " << outfile << "\n";
    if (!ask_yes_no("Continue?")) {
        log("Cancelled.");
        return;
    }

    HANDLE h = open_device(drive.c_str(), FALSE);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    uint64_t size = get_device_size(h);
    if (size == 0) {
        log("Error: Could not read the drive size.");
        CloseHandle(h);
        return;
    }
    size_t sectors = (size_t)(size / SECTOR_SIZE);

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
    bool failed = false;
    double startTime = (double)GetTickCount();

    while (written < sectors) {
        size_t todo = min(CHUNK, sectors - written);
        if (!read_sectors(h, written, buf, todo)) {
            log("Error: Read failed at sector " + to_string(written));
            failed = true;
            break;
        }
        if (fwrite(buf, SECTOR_SIZE, todo, out) != todo) {
            log("Error: Write to the image file failed.");
            failed = true;
            break;
        }
        written += todo;

        double elapsed = (GetTickCount() - startTime) / 1000.0;
        double mb = written * SECTOR_SIZE / 1024.0 / 1024.0;
        double speed = elapsed > 0 ? mb / elapsed : 0;
        cout << "\r  Written: " << fixed << setprecision(1) << mb << " MB  Speed: " << speed << " MB/s   " << flush;
    }

    if (fflush(out) != 0) failed = true;
    fclose(out);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);

    if (failed) {
        log("Backup stopped early — the image is incomplete.");
        return;
    }
    log("Done.");
}

void restore_drive(const string& infile, const string& rawDrive) {
    string drive = normalize_drive(rawDrive);
    if (!valid_disk_target(drive)) {
        log("Give a drive letter like D, or a disk number like 0.");
        return;
    }
    if (is_system_target(drive)) {
        log("Refusing: " + drive + " is the drive Windows is running from.");
        return;
    }

    log("Restoring " + infile + " to " + drive + ":...");

    cout << "\n⚠️  WARNING: This will ERASE ALL DATA on drive " << drive << ":!\n";
    cout << "Restoring from: " << infile << "\n";
    if (!ask_yes_no("Are you sure you want to continue?")) {
        log("Cancelled.");
        return;
    }

    FILE* in = fopen(infile.c_str(), "rb");
    if (!in) {
        log("Error: Cannot open input file");
        return;
    }

    HANDLE h = open_device(drive.c_str(), TRUE);
    if (h == INVALID_HANDLE_VALUE) {
        fclose(in);
        return;
    }

    _fseeki64(in, 0, SEEK_END);
    long long size = _ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);

    if (size <= 0) {
        log("Error: The image file is empty.");
        fclose(in);
        CloseHandle(h);
        return;
    }

    uint64_t deviceSize = get_device_size(h);
    if (deviceSize && (uint64_t)size > deviceSize) {
        log("Error: The image is bigger than the drive. Nothing was written.");
        fclose(in);
        CloseHandle(h);
        return;
    }

    size_t sectors = (size_t)(size / SECTOR_SIZE);
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
    bool failed = false;
    double startTime = (double)GetTickCount();

    while (written < sectors) {
        size_t todo = min(CHUNK, sectors - written);
        if (fread(buf, SECTOR_SIZE, todo, in) != todo) {
            log("Error: The image ended early. The drive may be partly written.");
            failed = true;
            break;
        }
        if (!write_sectors(h, written, buf, todo)) {
            log("Error: Write failed at sector " + to_string(written));
            failed = true;
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

    if (failed) {
        log("Restore stopped early.");
        return;
    }
    log("Done.");
}

void speed_test_drive(const string& rawDrive) {
    string drive = normalize_drive(rawDrive);
    if (!valid_disk_target(drive)) {
        log("Give a drive letter like D, or a disk number like 0.");
        return;
    }
    if (is_system_target(drive)) {
        log("Refusing: " + drive + " is the drive Windows is running from.");
        return;
    }

    log("Speed test for " + drive + ":...");
    cout << "\n⚠️  WARNING: This writes 256 MB to the start of drive " << drive
         << ": and ERASES the data there.\n";
    if (!ask_yes_no("Continue?")) {
        log("Cancelled.");
        return;
    }

    HANDLE h = open_device(drive.c_str(), TRUE);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    uint64_t size = get_device_size(h);
    if (size < 8 * 1024 * 1024) {
        log("Error: The drive is too small to test.");
        CloseHandle(h);
        return;
    }

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

    size_t chunks = min(TEST_SIZE / CHUNK, (size_t)(size / CHUNK));
    size_t bytes = chunks * CHUNK;
    bool failed = false;

    DWORD startTime = GetTickCount();
    for (size_t i = 0; i < chunks; i++) {
        if (!write_sectors(h, i * (CHUNK / SECTOR_SIZE), buf, CHUNK / SECTOR_SIZE)) {
            log("Error: Write failed during the speed test.");
            failed = true;
            break;
        }
    }
    FlushFileBuffers(h);
    double writeTime = (GetTickCount() - startTime) / 1000.0;

    DWORD readTime = 0;
    if (!failed) {
        startTime = GetTickCount();
        for (size_t i = 0; i < chunks; i++) {
            if (!read_sectors(h, i * (CHUNK / SECTOR_SIZE), buf, CHUNK / SECTOR_SIZE)) {
                log("Error: Read failed during the speed test.");
                failed = true;
                break;
            }
        }
        readTime = GetTickCount() - startTime;
    }

    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);
    if (failed) return;

    double writeSpeed = writeTime > 0 ? bytes / writeTime / 1024.0 / 1024.0 : 0;
    double readSpeed = readTime > 0 ? bytes / (readTime / 1000.0) / 1024.0 / 1024.0 : 0;

    ostringstream ss;
    ss << "  Write: " << fixed << setprecision(1) << writeSpeed
       << " MB/s  Read: " << readSpeed << " MB/s";
    log(ss.str());
}

void eject_drive(const string& rawDrive) {
    string drive = normalize_drive(rawDrive);
    if (!valid_drive_letter(drive)) {
        log("Eject needs a drive letter, like D.");
        return;
    }

    log("Ejecting " + drive + ":...");

    string cmd = "powershell -NoProfile -Command \"(New-Object -ComObject Shell.Application)"
                 ".Namespace(17).ParseName('" + drive + ":").InvokeVerb('Eject')\"";
    int rc = system(cmd.c_str());
    if (rc == 0) log("Done.");
    else log("Eject returned an error — close anything using the drive and try again.");
}

void print_usage(const char* prog) {
    cout << "=== cardnuke (Windows) ===\n\n";
    cout << "Usage: " << prog << " <drive> <mode> [options]\n\n";
    cout << "Drive:\n";
    cout << "  Letter (e.g., D) or number (0 for PhysicalDrive0)\n\n";
    cout << "Modes:\n";
    cout << "  1 format  - wipe/reformat (NTFS/FAT32/exFAT)\n";
    cout << "  2 backup  - image the drive to a .img file\n";
    cout << "  3 restore - flash a .img backup back to drive (erases the drive)\n";
    cout << "  4 speed   - benchmark read/write speed (erases the start of the drive)\n";
    cout << "  5 info    - show drive info\n";
    cout << "  6 eject   - safely remove drive\n";
    cout << "\nExamples:\n";
    cout << "  " << prog << " D 1             # format drive D:\n";
    cout << "  " << prog << " D 1 exfat mycard  # format D: as exFAT with label\n";
    cout << "  " << prog << " D 2 backup.img   # backup D: to file\n";
    cout << "  " << prog << " D 4             # speed test D: (asks first)\n";
    cout << "  " << prog << " 0 4              # speed test PhysicalDrive0\n";
    cout << "\nmade by Ivoryy06 ♡\n";
}

void print_menu() {
    cout << "\n=== cardnuke (Windows) ===\n\n";
    cout << "Choose an action:\n";
    cout << "  [1] Format    - Wipe and reformat drive\n";
    cout << "  [2] Backup    - Backup drive to .img file\n";
    cout << "  [3] Restore   - Restore .img file to drive (erases it)\n";
    cout << "  [4] Speed     - Benchmark read/write speed (erases the start)\n";
    cout << "  [5] Info      - Show drive information\n";
    cout << "  [6] Eject     - Safely remove drive\n";
    cout << "  [Q] Quit      - Exit\n";
    cout << "\nmade by Ivoryy06 ♡\n";
}

int main(int argc, char* argv[]) {
    cout << "=== cardnuke (Windows) ===\n";

    if (!is_admin()) {
        cout << "⚠️  Warning: Run as Administrator for full functionality\n\n";
    }

    if (argc < 2) {
        list_drives_windows();
        print_menu();
        
        string drive, mode;
        cout << "\nEnter drive letter (e.g., D): ";
        getline(cin, drive);
        if (drive.empty() || drive == "q" || drive == "Q") return 0;
        
        cout << "Choose mode (1-6): ";
        getline(cin, mode);
        
        if (mode == "1" || mode == "format" || mode == "Format") {
            string fs = ask_choice("Filesystem (NTFS/FAT32/exFAT)", "NTFS");
            string label = ask_choice("Volume label", "CARD");
            format_drive(drive, fs, label);
        } else if (mode == "2" || mode == "backup" || mode == "Backup") {
            string outfile = ask_choice("Backup filename", "backup.img");
            backup_drive(drive, outfile);
        } else if (mode == "3" || mode == "restore" || mode == "Restore") {
            string infile = ask_choice("Image filename", "backup.img");
            restore_drive(infile, drive);
        } else if (mode == "4" || mode == "speed" || mode == "Speed") {
            speed_test_drive(drive);
        } else if (mode == "5" || mode == "info" || mode == "Info") {
            list_drives_windows();
        } else if (mode == "6" || mode == "eject" || mode == "Eject") {
            eject_drive(drive);
        }
        
        return 0;
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