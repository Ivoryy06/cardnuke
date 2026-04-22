#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#ifdef __linux__
#include <linux/fs.h>
#include <mntent.h>
#include <sys/vfs.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#endif

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <cstdint>

using namespace std;

bool g_verbose = false;

string timestamp() {
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    struct tm* tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return string(buf);
}

void log(const string& msg) {
    string line = "[" + timestamp() + "] " + msg;
    cout << line << endl;
}

string exec(const string& cmd) {
    char buf[4096];
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    string result;
    while (fgets(buf, sizeof(buf), fp)) {
        result += buf;
    }
    pclose(fp);
    return result;
}

bool is_admin() {
#ifdef _WIN32
    return true;  // Check via IsUserAnAdmin()
#else
    return geteuid() == 0;
#endif
}

string get_partition(const string& dev, int index = 1) {
    if (dev.find("/dev/mmcblk") == 0) {
        return dev + "p" + to_string(index);
    }
    return dev + to_string(index);
}

bool mount_device(const string& dev, const string& mnt) {
#ifdef _WIN32
    return false;
#else
    string cmd = "mount -o sync " + dev + " " + mnt;
    return system(cmd.c_str()) == 0;
#endif
}

bool unmount_device(const string& mnt) {
#ifdef _WIN32
    return false;
#else
    string cmd = "umount " + mnt;
    return system(cmd.c_str()) == 0;
#endif
}

#ifndef _WIN32
void list_devices_linux() {
    vector<pair<string, string>> devs;

    DIR* dir = opendir("/sys/block");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir))) {
        string name = ent->d_name;
        if (name.substr(0, 2) == "sd" || name.substr(0, 4) == "nvme" || name.substr(0, 6) == "mmcblk") {
            string path = "/dev/" + name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;

            // Get size
            string size_path = "/sys/block/" + name + "/size";
            ifstream sz(size_path);
            long long sectors = 0;
            if (sz) sz >> sectors;
            long long bytes = sectors * 512;

            // Get model
            string model;
            string model_path = "/sys/block/" + name + "/device/model";
            ifstream mf(model_path);
            if (mf) {
                getline(mf, model);
                size_t start = model.find_first_not_of(" \t\n\r");
                size_t end = model.find_last_not_of(" \t\n\r");
                if (start != string::npos) model = model.substr(start, end - start + 1);
                else model = "Unknown";
            }

            // Get transport
            string tran;
            string tran_path = "/sys/block/" + name + "/device/transport";
            ifstream tf(tran_path);
            if (tf) getline(tf, tran);

            double gb = bytes / 1024.0 / 1024.0 / 1024.0;

            ostringstream ss;
            ss << "/dev/" << name << "  " << fixed << setprecision(1) << setw(6) << gb << "G  " 
               << setw(-30) << model << " " << tran;
            devs.push_back({name, ss.str()});
        }
    }
    closedir(dir);

    cout << "\nDEVICE       SIZE       MODEL                          TRAN     REMOVABLE\n";
    cout << "---------------------------------------------------------------------\n";
    for (auto& d : devs) {
        cout << d.second << "\n";
    }
}

string get_mounted_partition() {
    string result;
    FILE* mnt = setmntent("/proc/mounts", "r");
    if (mnt) {
        struct mntent* m;
        while ((m = getmntent(mnt))) {
            string fsname = m->mnt_fsname;
            if (fsname.find("/dev/mmcblk") == 0 || fsname.find("/dev/sd") == 0) {
                result = fsname;
                break;
            }
        }
        endmntent(mnt);
    }
    return result;
}
#endif

void list_devices() {
#ifdef _WIN32
    cout << "\nDEVICE       SIZE       MODEL                          TRAN     REMOVABLE\n";
    cout << "---------------------------------------------------------------------\n";
    system("wmic diskdrive get model,size,interfacetype /format:list 2>nul | findstr /v \"^-$\"");
#else
    list_devices_linux();
#endif
}

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void drop_caches() {
    sync();
#ifdef __linux__
    ofstream f("/proc/sys/vm/drop_caches");
    if (f) {
        f << "3";
        f.close();
    }
#endif
}

// SPEED TEST - uses raw block device to bypass filesystem limitations
void speed_test(const string& dev) {
    log("→ Speed test...");

#ifndef _WIN32
    string mnt = "/tmp/cardnuke_speed";
    string part = get_partition(dev);
    if (access(part.c_str(), F_OK) != 0) {
        part = dev;
    }

    // Unmount first to get raw access
    unmount_device(mnt);

    const size_t SIZE = 256 * 1024 * 1024;
    const size_t CHUNK = 8 * 1024 * 1024;

    char* buf = (char*)aligned_alloc(512, CHUNK);
    if (!buf) {
        log("  Error: cannot allocate buffer");
        return;
    }

    for (size_t i = 0; i < CHUNK; i++) {
        buf[i] = (i * 31) & 0xFF;
    }

    double write_speed = 0, read_speed = 0;
    int direct_ok = 0;

    // WRITE TEST - raw block device with O_DIRECT
    int fd = open(part.c_str(), O_RDWR | O_DIRECT);
    if (fd < 0) {
        // Try fallback without O_DIRECT
        fd = open(part.c_str(), O_RDWR);
        direct_ok = 0;
    } else {
        direct_ok = 1;
    }

    if (fd >= 0) {
        double t0 = get_time();
        size_t written = 0;
        while (written < SIZE) {
            ssize_t w = write(fd, buf, CHUNK);
            if (w <= 0) break;
            written += w;
        }
        fsync(fd);
        double t1 = get_time();
        write_speed = SIZE / (t1 - t0) / 1024 / 1024;
        close(fd);
    }

    // READ TEST
    drop_caches();
    usleep(100000);

    fd = open(part.c_str(), O_RDONLY);
    if (fd >= 0) {
        lseek(fd, 0, SEEK_SET);
        double t0 = get_time();
        size_t total = 0;
        while (total < SIZE) {
            ssize_t r = read(fd, buf, CHUNK);
            if (r <= 0) break;
            total += r;
        }
        double t1 = get_time();
        read_speed = SIZE / (t1 - t0) / 1024 / 1024;
        close(fd);
    }

    free(buf);

    ostringstream ss;
    ss << "  Write: " << fixed << setprecision(1) << write_speed << " MB/s  Read: " << read_speed << " MB/s (raw " << (direct_ok ? "O_DIRECT" : "buffered") << ")";
    log(ss.str());

#else
    log("  Speed test not supported on Windows yet");
#endif
}

// HEALTH CHECK
void health_check(const string& dev) {
    log("→ Running health / bad block check...");

#ifndef _WIN32
    string part = get_partition(dev);
    if (!part.empty() && access(part.c_str(), F_OK) != 0) {
        part = dev;
    }

    // Run badblocks
    string cmd = "badblocks -svw " + part;
    log("  Running badblocks read-write scan...");
    
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            cout << "  " << buf;
        }
        pclose(fp);
    }

    log("  Done.");
#else
    // Windows: use chkdsk
    string cmd = "chkdsk " + dev;
    system(cmd.c_str());
#endif
}

// FORMAT
void do_format(const string& dev, const string& fs, const string& label) {
    log("→ Formatting " + dev + " with " + fs + "...");

#ifndef _WIN32
    string part = get_partition(dev);
    
    // Unmount
    unmount_device("/tmp/cardnuke_format");
    rmdir("/tmp/cardnuke_format");
    mkdir("/tmp/cardnuke_format", 0755);

    // Wipe first 1MB
    log("  Wiping partition table...");
    int fd = open(part.c_str(), O_WRONLY);
    if (fd >= 0) {
        char buf[1024 * 1024] = {0};
        write(fd, buf, sizeof(buf));
        close(fd);
    }

    // Format
    log("  Creating " + fs + " filesystem...");
    string cmd = "mkfs." + fs + " -L \"" + label + "\" " + part;
    if (system(cmd.c_str()) != 0) {
        log("  Error: format failed");
        return;
    }

    log("  Done.");
#else
    log("  Format not implemented for Windows yet");
#endif
}

// BACKUP
void backup(const string& dev, const string& outfile) {
    log("→ Backing up " + dev + " to " + outfile + "...");

#ifndef _WIN32
    string cmd = "dd if=" + dev + " of=" + outfile + " bs=4M status=progress conv=fsync";
    if (system(cmd.c_str()) != 0) {
        log("  Error: backup failed");
        return;
    }

    // Calculate hash
    cmd = "sha256sum " + outfile + " > " + outfile + ".sha256";
    system(cmd.c_str());

    log("  Done.");
#else
    log("  Backup not implemented for Windows yet");
#endif
}

// RESTORE
void restore(const string& dev, const string& infile) {
    log("→ Restoring " + infile + " to " + dev + "...");

#ifndef _WIN32
    string cmd = "dd if=" + infile + " of=" + dev + " bs=4M status=progress conv=fsync";
    if (system(cmd.c_str()) != 0) {
        log("  Error: restore failed");
        return;
    }

    log("  Done.");
#else
    log("  Restore not implemented for Windows yet");
#endif
}

// INFO
void card_info(const string& dev) {
    log("→ Card info for " + dev + "...");

#ifndef _WIN32
    system(("lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL " + dev).c_str());
    system(("parted -s " + dev + " print").c_str());
#else
    system(("wmic diskdrive where \"DeviceID='" + dev + "' get model,size,serialnumber,bustype,healthstatus /format:list").c_str());
#endif
}

// EJECT
void eject(const string& dev) {
    log("→ Ejecting " + dev + "...");

#ifndef _WIN32
    string cmd = "eject " + dev;
    system(cmd.c_str());
#else
    string cmd = "powershell -Command \"(Get-WmiObject Win32_PhysicalDisk | Where-Object {$_.DeviceID -match '" + dev + "'}).Eject()\"";
    system(cmd.c_str());
#endif
}

#ifndef _WIN32
void repair(const string& dev) {
    log("→ Repairing " + dev + "...");
    
    string part = get_partition(dev);
    string mnt = "/tmp/cardnuke_repair";
    
    rmdir(mnt.c_str());
    mkdir(mnt.c_str(), 0755);
    
    mount_device(part, mnt);
    
    string cmd = "fsck.ext4 -fy " + part;
    system(cmd.c_str());
    
    unmount_device(mnt);
    rmdir(mnt.c_str());
    
    log("  Done.");
}
#endif

string ask_choice(const string& prompt, const string& def) {
    cout << prompt << " (default " << def << "): ";
    string r;
    getline(cin, r);
    if (r.empty()) return def;
    return r;
}

void print_menu() {
    cout << "\nMode:\n";
    cout << "  [1] format  - wipe/reformat\n";
    cout << "  [2] recover - recover files (PhotoRec)\n";
    cout << "  [3] health  - bad block + filesystem scan\n";
    cout << "  [4] backup  - image the card to a .img file\n";
    cout << "  [5] speed   - benchmark read/write speed\n";
    cout << "  [6] info    - show card info\n";
    cout << "  [7] repair  - fix corrupted filesystem (no wipe)\n";
    cout << "  [8] restore - flash a .img backup back to card\n";
    cout << "  [9] eject   - safely remove card\n";
}

int main(int argc, char* argv[]) {
    cout << "=== cardnuke (C++) ===\n";

    if (argc < 2) {
        list_devices();
        print_menu();
        
        string dev, mode;
        cout << "\nEnter device: ";
        getline(cin, dev);
        if (dev.empty()) return 0;
        
        cout << "Choose mode: ";
        getline(cin, mode);
        
        if (mode == "1" || mode == "format") {
            string fs = ask_choice("Filesystem (vfat/exfat/ext4)", "exfat");
            string label = ask_choice("Label", "EOS_DIGITAL");
            do_format(dev, fs, label);
        } else if (mode == "2") {
            // recover - invoke photorec
            log("→ Running PhotoRec...");
            system("photorec");
        } else if (mode == "3") {
            health_check(dev);
        } else if (mode == "4") {
            string outfile = ask_choice("Backup file", "/tmp/backup.img");
            backup(dev, outfile);
        } else if (mode == "5") {
            speed_test(dev);
        } else if (mode == "6") {
            card_info(dev);
        } else if (mode == "7") {
#ifndef _WIN32
            repair(dev);
#else
            log("  Repair not supported on Windows");
#endif
        } else if (mode == "8") {
            string infile = ask_choice("Image file", "/tmp/backup.img");
            restore(dev, infile);
        } else if (mode == "9") {
            eject(dev);
        }
        
        return 0;
    }

    // Command-line mode
    string dev = argv[1];
    string mode = argc > 2 ? argv[2] : "";

    if (mode == "format" || mode == "1") {
        string fs = argc > 3 ? argv[3] : "exfat";
        string label = argc > 4 ? argv[4] : "EOS_DIGITAL";
        do_format(dev, fs, label);
    } else if (mode == "health" || mode == "3") {
        health_check(dev);
    } else if (mode == "speed" || mode == "5") {
        speed_test(dev);
    } else if (mode == "backup" || mode == "4") {
        string outfile = argc > 3 ? argv[3] : "/tmp/backup.img";
        backup(dev, outfile);
    } else if (mode == "restore" || mode == "8") {
        string infile = argc > 3 ? argv[3] : "/tmp/backup.img";
        restore(dev, infile);
    } else if (mode == "info" || mode == "6") {
        card_info(dev);
    } else if (mode == "repair" || mode == "7") {
#ifndef _WIN32
        repair(dev);
#endif
    } else if (mode == "eject" || mode == "9") {
        eject(dev);
    } else if (mode.empty() || mode == "list") {
        list_devices();
    } else if (mode == "help" || mode == "-h") {
        print_menu();
    } else {
        cerr << "Unknown mode: " << mode << "\n";
        cerr << "Usage: " << argv[0] << " <device> <mode> [options]\n";
        cerr << "Modes: format, health, speed, backup, restore, info, repair, eject\n";
        return 1;
    }

    return 0;
}