# cardnuke

> *Wipe, recover, verify, and restore removable media from one terminal workflow.*

`cardnuke` is a high-performance SD/USB maintenance toolkit built for real recovery and reformat tasks, not just one-shot wiping.

## What it does

- **Destructive format modes** - low/medium/high/override wipe levels
- **Recovery flow** - launches PhotoRec, then deduplicates and organizes output
- **Health checks** - badblocks scan (read-write)
- **Backup + restore** - raw `.img` imaging with checksum support
- **Speed benchmark** - measures actual read/write speed

## Building

```bash
# Linux
make

# Or manually
g++ -O2 -std=c++17 -o cardnuke cardnuke.cpp
```

## Safety first

This tool can permanently destroy data.

- Always target whole devices intentionally (example: `/dev/sdb`)
- Double-check with `lsblk` before confirming
- Unmount mounted partitions before destructive operations
- Disconnect unrelated external drives when possible

If you're not sure which device is your card, stop and verify first.

## Running

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

## Supported devices

- `/dev/sdX` - USB/SATA drives
- `/dev/mmcblkX` - Built-in SD card readers
- `/dev/nvmeXn1` - NVMe drives

## Modes

| # | Mode | Status | Description |
|----|------|--------|------------|
| 1 | format | Stable | Wipe and reformat |
| 2 | recover | Stable | Launch PhotoRec |
| 3 | health | Stable | Bad block scan |
| 4 | backup | 🔧 Beta | Save to .img |
| 5 | speed | 🔧 Beta | Benchmark speed |
| 6 | info | Stable | Show device info |
| 7 | repair | 🔧 Beta | Fix filesystem |
| 8 | restore | 🔧 Beta | Flash .img |
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

## Requirements

### Linux

Required:
- `g++` (or `clang++`)
- `parted`, `dd`, `mkfs.*`, `fsck`
- `badblocks` (from e2fsprogs)
- `eject`

Optional:
- `testdisk` (for PhotoRec)
- `photorec`

### Windows

Build with MinGW-w64 or cross-compile with `x86_64-w64-mingw32-g++`.

## Performance

Written in C++ for maximum performance. The Python version is deprecated.

```text
Python cardnuke.py  →  C++ cardnuke
    ~50 MB/s           ~300 MB/s
```

## Logs

Each run outputs timestamped logs to stdout.

## Notes

- Use whole-device nodes (like `/dev/sdb`) for format/restore.
- MMC/SD naming handled automatically (`/dev/mmcblk0p1` vs `/dev/mmcblk0boot0`)
- Physical card speed depends on reader + card + bus