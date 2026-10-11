// cardnuke (Linux): wipe, recover, check, back up and restore SD cards and USB drives from one terminal.
//
// Safe by default:
//  - nothing destructive runs until you type the device path back (or pass --yes)
//  - speed and health checks leave your data alone unless you ask with --destructive
//  - it refuses to touch a disk that holds / /boot /home /usr /var, and warns about non-removable disks
//  - no shell is ever involved, so a strange device name or label can't run commands
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using std::string;
using std::vector;

struct Opts { bool yes = false, dry = false, destructive = false, allow_file = false, force = false, i_know = false; };
static Opts O;

enum { EXIT_OK = 0, EXIT_USAGE = 1, EXIT_REFUSED = 2, EXIT_FAILED = 3 };

static string stamp() {
    time_t t = time(nullptr);
    struct tm tmv; localtime_r(&t, &tmv);
    char buf[32]; strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}
static void say(const string& s) { std::cout << "[" << stamp() << "] " << s << std::endl; }
static void warn(const string& s) { std::cerr << "[" << stamp() << "] " << s << std::endl; }

static string join(const vector<string>& a) {
    string r;
    for (size_t i = 0; i < a.size(); i++) { if (i) r += ' '; r += a[i]; }
    return r;
}

// Runs a program with an argument list. No shell, so nothing in an argument can be interpreted as a command.
static int run(const vector<string>& args, const string& stdout_to = "") {
    if (O.dry) { say("  would run: " + join(args) + (stdout_to.empty() ? "" : "  > " + stdout_to)); return 0; }
    std::cout.flush(); std::cerr.flush();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!stdout_to.empty()) {
            int fd = open(stdout_to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(126);
            dup2(fd, 1); close(fd);
        }
        vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static string capture(const vector<string>& args) {
    int p[2];
    if (pipe(p) != 0) return "";
    std::cout.flush();
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return ""; }
    if (pid == 0) {
        close(p[0]); dup2(p[1], 1); close(p[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, 2); close(dn); }
        vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(p[1]);
    string out; char buf[512]; ssize_t n;
    while ((n = read(p[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    close(p[0]);
    int st = 0; waitpid(pid, &st, 0);
    return out;
}

static string trim(string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}
static string slurp1(const string& path) { std::ifstream f(path); string s; if (f) std::getline(f, s); return trim(s); }
static string base_name(const string& p) { size_t i = p.find_last_of('/'); return i == string::npos ? p : p.substr(i + 1); }
static string fmt_gb(unsigned long long b) { std::ostringstream o; o << std::fixed << std::setprecision(1) << b / 1073741824.0 << "G"; return o.str(); }

// ---------------------------------------------------------------- devices

struct Dev { string path, name; bool is_file = false; unsigned long long bytes = 0; };

static unsigned long long size_of(int fd, const struct stat& st) {
    if (S_ISBLK(st.st_mode)) { unsigned long long b = 0; if (ioctl(fd, BLKGETSIZE64, &b) == 0) return b; return 0; }
    return (unsigned long long)st.st_size;
}

static bool load_device(const string& p, Dev& d, string& err) {
    struct stat st;
    if (p.empty() || stat(p.c_str(), &st) != 0) { err = "can't find " + p; return false; }
    if (S_ISBLK(st.st_mode)) {
        if (p.rfind("/dev/", 0) != 0) { err = "device nodes must live under /dev"; return false; }
        d.is_file = false;
        d.name = base_name(p);
        if (access(("/sys/block/" + d.name).c_str(), F_OK) != 0) {
            err = p + " looks like a partition. Give the whole device instead, for example /dev/sdb or /dev/mmcblk0.";
            return false;
        }
    } else if (S_ISREG(st.st_mode) && O.allow_file) {
        d.is_file = true; d.name = base_name(p);
    } else {
        err = p + " is not a block device" + (S_ISREG(st.st_mode) ? " (use --allow-file to work on a disk image file)" : "");
        return false;
    }
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) { err = "can't open " + p + ": " + strerror(errno); return false; }
    d.path = p; d.bytes = size_of(fd, st);
    close(fd);
    return true;
}

static string part_path(const Dev& d, int n) {
    string s = d.path;
    return s + (!s.empty() && isdigit((unsigned char)s.back()) ? "p" : "") + std::to_string(n);
}

struct Mount { string src, dir; };
static vector<Mount> mounts_of(const Dev& d) {
    vector<Mount> r;
    if (d.is_file) return r;
    std::ifstream f("/proc/mounts"); string src, dir, rest;
    while (f >> src >> dir) {
        std::getline(f, rest);
        if (src.rfind(d.path, 0) != 0) continue;
        string tail = src.substr(d.path.size());
        bool ok = tail.empty();
        if (!ok) { size_t i = (tail[0] == 'p') ? 1 : 0; ok = i < tail.size() && std::all_of(tail.begin() + i, tail.end(), ::isdigit); }
        if (ok) {
            // /proc/mounts escapes spaces as \040
            string out; for (size_t i = 0; i < dir.size(); i++) { if (dir.compare(i, 4, "\\040") == 0) { out += ' '; i += 3; } else out += dir[i]; }
            r.push_back({src, out});
        }
    }
    return r;
}

static string describe(const Dev& d) {
    string model = d.is_file ? "disk image file" : slurp1("/sys/block/" + d.name + "/device/model");
    if (model.empty()) model = "unknown model";
    return d.path + "  " + fmt_gb(d.bytes) + "  " + model;
}

// Refuses system disks; warns about non-removable ones. Returns false to stop.
static bool safety_check(const Dev& d) {
    static const char* system_dirs[] = {"/", "/boot", "/boot/efi", "/home", "/usr", "/var", "/etc", "/opt", "/srv"};
    for (auto& m : mounts_of(d))
        for (auto s : system_dirs)
            if (m.dir == s) { warn("Refusing: " + d.path + " holds " + m.dir + ", which is part of your running system."); return false; }
    if (d.is_file) return true;
    char real[PATH_MAX]; string sys = "/sys/block/" + d.name;
    bool usb = realpath(sys.c_str(), real) && string(real).find("/usb") != string::npos;
    bool mmc = d.name.rfind("mmcblk", 0) == 0;
    bool removable = slurp1(sys + "/removable") == "1";
    if (!(usb || mmc || removable) && !O.i_know) {
        warn("Refusing: " + d.path + " doesn't look like a removable card or USB drive (it may be an internal disk).");
        warn("If you're certain, run again with --i-know.");
        return false;
    }
    return true;
}

static bool unmount_all(const Dev& d) {
    for (auto& m : mounts_of(d)) {
        say("  unmounting " + m.src);
        if (run({"umount", m.src}) != 0) { warn("Couldn't unmount " + m.src + ". Close anything using it and try again."); return false; }
    }
    return true;
}

static bool confirm_destroy(const Dev& d, const string& what) {
    std::cout << "\n  " << what << "\n  Target: " << describe(d) << "\n";
    for (auto& m : mounts_of(d)) std::cout << "  Mounted: " << m.src << " at " << m.dir << "\n";
    if (O.dry || O.yes) return true;
    if (!isatty(0)) { warn("Refusing: this changes data and there is no terminal to confirm on. Add --yes if you really mean it."); return false; }
    std::cout << "  Type the device path (" << d.path << ") to continue: ";
    string a; std::getline(std::cin, a);
    if (a != d.path) { warn("That didn't match. Nothing was changed."); return false; }
    return true;
}

// ---------------------------------------------------------------- modes

static double now_s() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec / 1e9; }
static string mbps(unsigned long long bytes, double secs) {
    std::ostringstream o; o << std::fixed << std::setprecision(1) << (secs > 0 ? bytes / secs / 1048576.0 : 0.0) << " MB/s"; return o.str();
}

static int cmd_list() {
    std::cout << "\nDEVICE        SIZE    MODEL                     TRAN   REMOVABLE\n";
    std::cout << "----------------------------------------------------------------\n";
    DIR* dir = opendir("/sys/block");
    if (!dir) return EXIT_FAILED;
    vector<string> names; struct dirent* e;
    while ((e = readdir(dir))) {
        string n = e->d_name;
        if (n.rfind("sd", 0) == 0 || n.rfind("nvme", 0) == 0 || n.rfind("mmcblk", 0) == 0) names.push_back(n);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        unsigned long long sectors = strtoull(slurp1("/sys/block/" + n + "/size").c_str(), nullptr, 10);
        string model = slurp1("/sys/block/" + n + "/device/model"); if (model.empty()) model = "-";
        string tran = slurp1("/sys/block/" + n + "/device/transport"); if (tran.empty()) tran = "-";
        string rem = slurp1("/sys/block/" + n + "/removable") == "1" ? "yes" : "no";
        std::cout << std::left << std::setw(14) << ("/dev/" + n) << std::right << std::setw(7) << fmt_gb(sectors * 512) << "  "
                  << std::left << std::setw(26) << model.substr(0, 25) << std::setw(7) << tran << rem << "\n";
    }
    if (names.empty()) std::cout << "(no disks found)\n";
    return EXIT_OK;
}

static int cmd_info(const Dev& d) {
    say("Card info for " + d.path);
    std::cout << "  " << describe(d) << "\n";
    run({"lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL", d.path});
    run({"parted", "-s", d.path, "print"});
    return EXIT_OK;
}

// Read speed straight from the device; write speed through a temporary file on the card if it is mounted.
static int cmd_speed(const Dev& d) {
    say("Speed test on " + d.path + (O.destructive ? " (destructive raw write test)" : " (your data is left alone)"));
    const size_t CHUNK = 8u << 20;
    const unsigned long long LIMIT = 256ull << 20;
    void* raw = nullptr;
    if (posix_memalign(&raw, 4096, CHUNK) != 0) { warn("Out of memory."); return EXIT_FAILED; }
    char* buf = (char*)raw;
    for (size_t i = 0; i < CHUNK; i++) buf[i] = (char)((i * 31) & 0xFF);

    unsigned long long want = d.bytes ? std::min<unsigned long long>(LIMIT, d.bytes) : LIMIT;
    int fd = open(d.path.c_str(), O_RDONLY | O_DIRECT);
    bool direct = fd >= 0;
    if (fd < 0) fd = open(d.path.c_str(), O_RDONLY);
    if (fd < 0) { warn("Can't read " + d.path + ": " + strerror(errno)); free(raw); return EXIT_FAILED; }
    double t0 = now_s(); unsigned long long total = 0;
    while (total < want) {
        ssize_t r = read(fd, buf, (size_t)std::min<unsigned long long>(CHUNK, want - total));
        if (r <= 0) break;
        total += (unsigned long long)r;
    }
    double t1 = now_s(); close(fd);
    say("  Read:  " + mbps(total, t1 - t0) + (direct ? "  (direct, bypasses the cache)" : "  (buffered, may look faster than the card really is)"));

    if (O.destructive) {
        if (!confirm_destroy(d, "The raw write test overwrites the first 256 MB of the device and destroys the data there.")) { free(raw); return EXIT_REFUSED; }
        if (!unmount_all(d)) { free(raw); return EXIT_FAILED; }
        if (!O.dry) {
            int w = open(d.path.c_str(), O_WRONLY | O_DIRECT);
            if (w < 0) w = open(d.path.c_str(), O_WRONLY);
            if (w < 0) { warn("Can't write " + d.path + ": " + strerror(errno)); free(raw); return EXIT_FAILED; }
            double w0 = now_s(); unsigned long long done = 0;
            while (done < want) { ssize_t r = write(w, buf, (size_t)std::min<unsigned long long>(CHUNK, want - done)); if (r <= 0) break; done += (unsigned long long)r; }
            fsync(w); double w1 = now_s(); close(w);
            say("  Write: " + mbps(done, w1 - w0) + "  (raw)");
        } else say("  would run the raw write test");
    } else {
        auto ms = mounts_of(d);
        if (ms.empty()) {
            say("  Write: skipped. Mount the card first and run again, or use --destructive to test the raw device.");
        } else {
            string tmp = ms[0].dir + "/.cardnuke_speedtest.tmp";
            struct statvfs sv;
            unsigned long long freeb = statvfs(ms[0].dir.c_str(), &sv) == 0 ? (unsigned long long)sv.f_bavail * sv.f_frsize : 0;
            unsigned long long wsize = std::min<unsigned long long>(128ull << 20, freeb / 2);
            if (wsize < CHUNK) { say("  Write: skipped, not enough free space on " + ms[0].dir); }
            else {
                int w = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
                if (w < 0) w = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (w < 0) { say(string("  Write: skipped, can't write to ") + ms[0].dir + ": " + strerror(errno)); }
                else {
                    double w0 = now_s(); unsigned long long done = 0;
                    while (done < wsize) { ssize_t r = write(w, buf, CHUNK); if (r <= 0) break; done += (unsigned long long)r; }
                    fsync(w); double w1 = now_s(); close(w); unlink(tmp.c_str());
                    say("  Write: " + mbps(done, w1 - w0) + "  (temporary file on " + ms[0].dir + ", removed afterwards)");
                }
            }
        }
    }
    free(raw);
    return EXIT_OK;
}

static int cmd_health(const Dev& d) {
    if (O.destructive) {
        say("Read-write bad block scan on " + d.path);
        if (!confirm_destroy(d, "The read-write scan writes test patterns over every block and DESTROYS all data on the device.")) return EXIT_REFUSED;
        if (!unmount_all(d)) return EXIT_FAILED;
        int rc = run({"badblocks", "-wsv", d.path});
        if (rc == 127) { warn("badblocks isn't installed (it comes with e2fsprogs)."); return EXIT_FAILED; }
        return rc == 0 ? EXIT_OK : EXIT_FAILED;
    }
    say("Read-only bad block scan on " + d.path + " (your data is left alone)");
    if (!unmount_all(d)) return EXIT_FAILED;
    int rc = run({"badblocks", "-sv", d.path});
    if (rc == 127) { warn("badblocks isn't installed (it comes with e2fsprogs)."); return EXIT_FAILED; }
    return rc == 0 ? EXIT_OK : EXIT_FAILED;
}

static bool valid_label(const string& s) {
    if (s.empty() || s.size() > 11) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c == ' '; });
}

static int cmd_format(const Dev& d, const string& fs, const string& label) {
    if (fs != "vfat" && fs != "exfat" && fs != "ext4") { warn("Filesystem must be vfat, exfat or ext4."); return EXIT_USAGE; }
    if (!valid_label(label)) { warn("The label can be up to 11 letters, numbers, spaces, - or _."); return EXIT_USAGE; }
    say("Formatting " + d.path + " as " + fs + " (label " + label + ")");
    if (!confirm_destroy(d, "Formatting ERASES everything on the device.")) return EXIT_REFUSED;
    if (!unmount_all(d)) return EXIT_FAILED;

    string target = d.path;
    if (!d.is_file) {
        say("  clearing the old partition table");
        if (!O.dry) {
            int fd = open(d.path.c_str(), O_WRONLY);
            if (fd < 0) { warn("Can't open " + d.path + " for writing: " + strerror(errno)); return EXIT_FAILED; }
            vector<char> zeros(1u << 20, 0);
            ssize_t w = write(fd, zeros.data(), zeros.size()); (void)w;
            fsync(fd); close(fd);
        }
        string pfs = fs == "vfat" ? "fat32" : fs == "exfat" ? "ntfs" : "ext4";   // "ntfs" gives the MBR type used by exFAT
        say("  creating one partition");
        if (run({"parted", "-s", d.path, "mklabel", "msdos", "mkpart", "primary", pfs, "1MiB", "100%"}) != 0) { warn("parted failed."); return EXIT_FAILED; }
        run({"partprobe", d.path}); run({"udevadm", "settle"});
        target = part_path(d, 1);
        for (int i = 0; i < 50 && !O.dry && access(target.c_str(), F_OK) != 0; i++) usleep(100000);
        if (!O.dry && access(target.c_str(), F_OK) != 0) { warn("The new partition " + target + " didn't appear."); return EXIT_FAILED; }
    } else say("  (disk image file: skipping the partition table, formatting the file directly)");

    say("  creating the " + fs + " filesystem");
    vector<string> mk = fs == "vfat"  ? vector<string>{"mkfs.vfat", "-F", "32", "-n", label, target}
                      : fs == "exfat" ? vector<string>{"mkfs.exfat", "-L", label, target}
                                      : vector<string>{"mkfs.ext4", "-F", "-L", label, target};
    int rc = run(mk);
    if (rc == 127) { warn(mk[0] + " isn't installed."); return EXIT_FAILED; }
    if (rc != 0) { warn("Formatting failed."); return EXIT_FAILED; }
    say("  Done.");
    return EXIT_OK;
}

static int cmd_backup(const Dev& d, const string& out) {
    if (out.rfind("/dev/", 0) == 0) { warn("Choose a file for the backup, not a device."); return EXIT_USAGE; }
    struct stat st;
    if (stat(out.c_str(), &st) == 0 && !O.force && !O.dry) { warn(out + " already exists. Use --force to overwrite it."); return EXIT_REFUSED; }
    say("Backing up " + d.path + " to " + out);
    if (!unmount_all(d)) return EXIT_FAILED;
    int rc = run({"dd", "if=" + d.path, "of=" + out, "bs=4M", "status=progress", "conv=fsync"});
    if (rc != 0) { warn("Backup failed."); return EXIT_FAILED; }
    say("  writing the checksum to " + out + ".sha256");
    run({"sha256sum", out}, out + ".sha256");
    say("  Done.");
    return EXIT_OK;
}

static int cmd_restore(const Dev& d, const string& in) {
    struct stat st;
    if (stat(in.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { warn("Can't find the image file " + in); return EXIT_USAGE; }
    if (d.bytes && (unsigned long long)st.st_size > d.bytes) { warn("The image (" + fmt_gb((unsigned long long)st.st_size) + ") is bigger than the device (" + fmt_gb(d.bytes) + ")."); return EXIT_REFUSED; }
    string sumfile = in + ".sha256";
    if (access(sumfile.c_str(), R_OK) == 0 && !O.dry) {
        say("  checking the image against " + sumfile);
        string want = trim(slurp1(sumfile)); want = want.substr(0, want.find(' '));
        string got = trim(capture({"sha256sum", in})); got = got.substr(0, got.find(' '));
        if (want.empty() || got != want) { warn("The image doesn't match its checksum. It may be damaged. Nothing was written."); return EXIT_REFUSED; }
        say("  checksum matches");
    }
    say("Restoring " + in + " to " + d.path);
    if (!confirm_destroy(d, "Restoring ERASES everything on the device and replaces it with the image.")) return EXIT_REFUSED;
    if (!unmount_all(d)) return EXIT_FAILED;
    if (run({"dd", "if=" + in, "of=" + d.path, "bs=4M", "status=progress", "conv=fsync"}) != 0) { warn("Restore failed."); return EXIT_FAILED; }
    run({"sync"});
    say("  Done.");
    return EXIT_OK;
}

static int cmd_repair(const Dev& d) {
    say("Repairing the filesystem on " + d.path + " (files are kept)");
    if (d.is_file) { warn("Repair works on real devices only."); return EXIT_USAGE; }
    if (!confirm_destroy(d, "Repair rewrites filesystem structures. Back up first if the files matter.")) return EXIT_REFUSED;
    if (!unmount_all(d)) return EXIT_FAILED;
    int fixed = 0, found = 0;
    for (int i = 1; i <= 4; i++) {
        string part = part_path(d, i);
        if (access(part.c_str(), F_OK) != 0) continue;
        found++;
        string type = trim(capture({"blkid", "-o", "value", "-s", "TYPE", part}));
        vector<string> cmd;
        if (type == "vfat") cmd = {"fsck.vfat", "-a", part};
        else if (type == "exfat") cmd = {"fsck.exfat", "-p", part};
        else if (type == "ext2" || type == "ext3" || type == "ext4") cmd = {"e2fsck", "-fp", part};
        else { say("  " + part + ": filesystem \"" + type + "\" isn't supported, skipping"); continue; }
        say("  checking " + part + " (" + type + ")");
        int rc = run(cmd);
        if (rc == 127) { warn(cmd[0] + " isn't installed."); continue; }
        if (rc <= 1) fixed++;   // 0 = clean, 1 = errors were fixed
        else warn("  " + part + ": the checker reported a problem it could not fix (exit " + std::to_string(rc) + ")");
    }
    if (!found) { warn("No partitions found on " + d.path + "."); return EXIT_FAILED; }
    say("  Done.");
    return fixed ? EXIT_OK : EXIT_FAILED;
}

static int cmd_eject(const Dev& d) {
    say("Ejecting " + d.path);
    if (!unmount_all(d)) return EXIT_FAILED;
    run({"sync"});
    int rc = run({"eject", d.path});
    if (rc == 127) { warn("eject isn't installed."); return EXIT_FAILED; }
    return rc == 0 ? EXIT_OK : EXIT_FAILED;
}

static int cmd_recover(const Dev& d) {
    say("Starting PhotoRec on " + d.path + " (it only reads, so your card is not changed)");
    int rc = run({"photorec", d.path});
    if (rc == 127) { warn("photorec isn't installed (it comes with testdisk)."); return EXIT_FAILED; }
    return rc == 0 ? EXIT_OK : EXIT_FAILED;
}

// ---------------------------------------------------------------- command line

static void print_modes() {
    std::cout <<
        "\nModes:\n"
        "  [1] format   erase the card and make a fresh filesystem   (destructive)\n"
        "  [2] recover  recover deleted files with PhotoRec          (read-only)\n"
        "  [3] health   bad block scan                               (read-only; --destructive for a read-write scan)\n"
        "  [4] backup   save the card to a .img file with a checksum (read-only)\n"
        "  [5] speed    read and write benchmark                     (safe; --destructive for a raw write test)\n"
        "  [6] info     show card details\n"
        "  [7] repair   fix a damaged filesystem, keeping the files\n"
        "  [8] restore  write a .img backup back to the card         (destructive)\n"
        "  [9] eject    unmount and eject safely\n";
}

static void usage(const char* me) {
    std::cout << "Usage: " << me << " [options] <device> <mode> [mode arguments]\n"
              << "       " << me << " list\n"
              << "\nExamples:\n"
              << "  sudo " << me << " /dev/sdb speed\n"
              << "  sudo " << me << " /dev/sdb format exfat MYCARD\n"
              << "  sudo " << me << " /dev/sdb backup card.img\n"
              << "  sudo " << me << " /dev/sdb restore card.img\n"
              << "\nOptions:\n"
              << "  --dry-run      show what would run and change nothing\n"
              << "  --yes          skip the \"type the device path\" confirmation\n"
              << "  --destructive  allow the read-write health scan and raw write speed test\n"
              << "  --force        overwrite an existing backup file\n"
              << "  --allow-file   work on a disk image file instead of a device (for testing)\n"
              << "  --i-know       allow a disk that doesn't look removable\n";
    print_modes();
}

static string canon_mode(const string& m) {
    static const char* names[] = {"", "format", "recover", "health", "backup", "speed", "info", "repair", "restore", "eject"};
    if (m.size() == 1 && m[0] >= '1' && m[0] <= '9') return names[m[0] - '0'];
    return m;
}

static int dispatch(const string& devpath, const string& mode, const vector<string>& extra) {
    if (mode == "list") return cmd_list();
    Dev d; string err;
    if (!load_device(devpath, d, err)) { warn(err); return EXIT_USAGE; }
    if (geteuid() != 0 && !O.dry && !d.is_file) { warn("Run this with sudo: it needs root to work on a device."); return EXIT_REFUSED; }
    if (!safety_check(d)) return EXIT_REFUSED;

    if (mode == "format")  return cmd_format(d, extra.size() > 0 ? extra[0] : "exfat", extra.size() > 1 ? extra[1] : "EOS_DIGITAL");
    if (mode == "recover") return cmd_recover(d);
    if (mode == "health")  return cmd_health(d);
    if (mode == "backup")  return cmd_backup(d, extra.size() > 0 ? extra[0] : "/tmp/backup.img");
    if (mode == "speed")   return cmd_speed(d);
    if (mode == "info")    return cmd_info(d);
    if (mode == "repair")  return cmd_repair(d);
    if (mode == "restore") return cmd_restore(d, extra.size() > 0 ? extra[0] : "/tmp/backup.img");
    if (mode == "eject")   return cmd_eject(d);
    warn("Unknown mode: " + mode);
    usage("cardnuke");
    return EXIT_USAGE;
}

static string ask(const string& q, const string& def = "") {
    std::cout << q << (def.empty() ? "" : " (default " + def + ")") << ": ";
    string r; std::getline(std::cin, r);
    r = trim(r);
    return r.empty() ? def : r;
}

int main(int argc, char** argv) {
    vector<string> pos;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--yes" || a == "-y") O.yes = true;
        else if (a == "--dry-run") O.dry = true;
        else if (a == "--destructive") O.destructive = true;
        else if (a == "--force") O.force = true;
        else if (a == "--allow-file") O.allow_file = true;
        else if (a == "--i-know") O.i_know = true;
        else if (a == "--list") pos = {"list"};
        else if (a == "-h" || a == "--help" || a == "help") { usage(argv[0]); return EXIT_OK; }
        else if (a.size() > 1 && a[0] == '-' && a[1] == '-') { warn("Unknown option " + a); usage(argv[0]); return EXIT_USAGE; }
        else pos.push_back(a);
    }
    std::cout << "=== cardnuke ===\n";

    if (pos.empty()) {
        cmd_list(); print_modes();
        string dev = ask("\nEnter device");
        if (dev.empty()) return EXIT_OK;
        string mode = canon_mode(ask("Choose mode"));
        vector<string> extra;
        if (mode == "format") { extra.push_back(ask("Filesystem (vfat/exfat/ext4)", "exfat")); extra.push_back(ask("Label", "EOS_DIGITAL")); }
        else if (mode == "backup") extra.push_back(ask("Backup file", "/tmp/backup.img"));
        else if (mode == "restore") extra.push_back(ask("Image file", "/tmp/backup.img"));
        return dispatch(dev, mode, extra);
    }
    if (pos[0] == "list") return cmd_list();
    if (pos.size() < 2) { usage(argv[0]); return EXIT_USAGE; }
    vector<string> extra(pos.begin() + 2, pos.end());
    return dispatch(pos[0], canon_mode(pos[1]), extra);
}
