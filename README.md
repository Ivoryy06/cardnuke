# cardnuke

> *Wipe, recover, verify, and restore removable media from one terminal workflow.*

`cardnuke` is a high-performance SD/USB maintenance toolkit built for real recovery and reformat tasks, not just one-shot wiping.

## What it does

- **Destructive format modes** - low/medium/high/override wipe levels
- **Recovery flow** - launches PhotoRec, then deduplicates and organizes output
- **Health checks** - badblocks scan (read-write)
- **Backup + restore** - raw `.img` imaging with checksum support
- **Speed benchmark** - measures actual read/write speed

## Requirements

### Linux

Build requirements:
- `g++` (C++17 compiler)

Runtime requirements:
- `parted`, `dd`, `mkfs.*`, `fsck`
- `badblocks` (from e2fsprogs)
- `eject`
- `sudo` (for root access)

Optional:
- `testdisk` / `photorec` (for file recovery)

### Windows

Build requirements:
- MinGW-w64 with C++17 support
- `g++ -O2 -Wall -std=c++17 -o cardnuke_win.exe cardnuke_win.cpp -lws2_32 -lsetupapi`

Runtime requirements:
- **Administrator privileges** (right-click CMD → Run as administrator)
- No external dependencies - uses Windows API directly

## Building

```bash
# Linux
make linux

# Or manually
g++ -O2 -Wall -std=c++17 -o cardnuke cardnuke.cpp
```

## Installation

### Linux

```bash
sudo make install
# Or manually
sudo cp cardnuke /usr/local/bin/
```

### Windows

1. Compile with MinGW-w64:
```cmd
g++ -O2 -Wall -std=c++17 -o cardnuke_win.exe cardnuke_win.cpp -lws2_32 -lsetupapi
```

2. Copy to a folder in PATH or run from its location:
```cmd
copy cardnuke_win.exe C:\Windows\System32\
```

## Running

### Linux

```bash
# Interactive mode
sudo ./cardnuke

# Command-line mode
sudo ./cardnuke <device> <mode> [options]

# Examples
sudo ./cardnuke /dev/sdb 5           # speed test
sudo ./cardnuke /dev/sdb 1 exfat     # format with exfat
sudo ./cardnuke /dev/sdb 4 backup.img  # backup to file
sudo ./cardnuke /dev/sdb 3           # health check
```

### Windows

Open **Command Prompt as Administrator**:

```cmd
# List drives
cardnuke_win.exe --list

# Format (NTFS/FAT32/exFAT)
cardnuke_win.exe D 1 NTFS mycard

# Backup to .img file
cardnuke_win.exe D 2 backup.img

# Speed test
cardnuke_win.exe D 4

# Eject safely
cardnuke_win.exe D 6
```

## Supported devices

### Linux
- `/dev/sdX` - USB/SATA drives
- `/dev/mmcblkX` - Built-in SD card readers
- `/dev/nvmeXn1` - NVMe drives

### Windows
- Drive letter (e.g., `D`, `E`)
- PhysicalDrive number (e.g., `0`, `1`)

## Modes

| # | Mode | Status | Description |
|----|------|--------|------------|
| 1 | format | Stable | Wipe and reformat |
| 2 | recover | Stable | Launch PhotoRec |
| 3 | health | Stable | Bad block scan |
| 4 | backup | Stable | Save to .img |
| 5 | speed | Stable | Benchmark speed |
| 6 | info | Stable | Show device info |
| 7 | repair | Stable | Fix filesystem |
| 8 | restore | Stable | Flash .img |
| 9 | eject | Stable | Safely remove |

## Speed test details

The speed test measures actual hardware throughput:

- **Write**: 8MB sequential blocks, fsync for hardware sync
- **Read**: Unbuffered with cache drop
- Uses `O_DIRECT` when available for true speeds
- Test size: 256MB

Results reflect your actual reader/card combination:
- UHS-I: ~50-100 MB/s
- UHS-II: ~150-300 MB/s
- USB 2.0: ~20-40 MB/s
- USB 3.0: ~100-200 MB/s

## Performance

Written in C++ for maximum performance.

```text
Python cardnuke.py  →  C++ cardnuke
    ~50 MB/s           ~300 MB/s
```

## Safety first

This tool can permanently destroy data.

- Always target whole devices intentionally
- Double-check with `lsblk` (Linux) or `--list` (Windows) before confirming
- Unmount mounted partitions before destructive operations
- Disconnect unrelated external drives when possible

If you're not sure which device is your card, stop and verify first.

## Notes

- Use whole-device nodes (like `/dev/sdb`) for format/restore on Linux
- On Windows, use drive letters without `:` (e.g., `D` not `D:`)
- MMC/SD naming handled automatically (`/dev/mmcblk0p1` vs `/dev/mmcblk0boot0`)
- Physical card speed depends on reader + card + bus