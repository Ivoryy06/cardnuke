#include "cardnuke.h"
#include <iostream>
#include <string>
#include <vector>
#include <map>

void print_usage(const char* prog) {
    std::cout << "=== cardnuke ===\n\n";
    std::cout << "Usage: " << prog << " <device> <mode> [options]\n\n";
    std::cout << "Modes:\n";
    std::cout << "  1 format  - wipe/reformat\n";
    std::cout << "  2 recover - recover files (PhotoRec)\n";
    std::cout << "  3 health  - bad block + filesystem scan\n";
    std::cout << "  4 backup  - image the card to a .img file\n";
    std::cout << "  5 speed   - benchmark read/write speed\n";
    std::cout << "  6 info    - show card info\n";
    std::cout << "  7 repair  - fix corrupted filesystem\n";
    std::cout << "  8 restore - flash a .img backup back to card\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << prog << " /dev/sdb 1         # format /dev/sdb\n";
    std::cout << "  " << prog << " /dev/mmcblk0 5    # speed test\n";
    std::cout << "  " << prog << " /dev/sdb 4 backup.img  # backup to file\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        list_devices();
        print_usage(argv[0]);
        return 1;
    }

    std::string dev = argv[1];
    std::string mode = argv[2];

    log("=== cardnuke started ===");

    if (mode == "1") {
        // format
        return 0;
    } else if (mode == "2") {
        // recover
        return 0;
    } else if (mode == "3") {
        // health check
        return 0;
    } else if (mode == "4") {
        // backup
        return 0;
    } else if (mode == "5") {
        // speed test
        return 0;
    } else if (mode == "6") {
        // info
        return 0;
    } else if (mode == "7") {
        // repair
        return 0;
    } else if (mode == "8") {
        // restore
        return 0;
    }

    std::cerr << "Unknown mode: " << mode << "\n";
    return 1;
}