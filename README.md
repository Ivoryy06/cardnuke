# cardnuke

> *Wipe, recover, verify, and restore removable media from one terminal workflow.*

`cardnuke` is a practical SD/USB maintenance toolkit built for real recovery and reformat tasks, not just one-shot wiping.

## What it does

- **Destructive format modes** - low/medium/high/override wipe levels
- **Recovery flow** - launches PhotoRec, then deduplicates and organizes output
- **Health checks** - S.M.A.R.T. summary + badblocks scan
- **Backup + restore** - raw `.img` imaging with checksum support
- **Post-format confidence checks** - quick write verification and speed test

## Safety first

This tool can permanently destroy data.

- Always target whole devices intentionally (example: `/dev/sdb`)
- Double-check with `lsblk` before confirming `YES`
- Unmount mounted partitions before destructive operations
- Disconnect unrelated external drives when possible

If you're not sure which device is your card, stop and verify first.

## Project structure

```text
cardnuke/
├── cardnuke.py   # Main interactive toolkit (Linux + Windows terminal-native)
├── cardnuke.sh   # Linux quick-format helper
└── README.md
```

## Stack

| Layer | Tooling |
|---|---|
| Core runtime | Python 3 + Bash |
| Disk ops | `dd`, `parted`, `mkfs.*`, `fsck`, `mount/umount` |
| Recovery | `photorec` (from `testdisk`) |
| Health | `smartctl` (optional), `badblocks` |
| UX extras | `notify-send`, `paplay`, `eject` (optional) |

## Requirements

### Linux

Required/common commands:

- `lsblk`, `mount`, `umount`, `parted`, `dd`, `blkid`, `fsck`
- `mkfs.vfat` (`dosfstools`)
- `mkfs.exfat` (`exfatprogs`) for exFAT
- `mkfs.ext4` (`e2fsprogs`) for ext4

Optional but recommended:

- `testdisk` (provides `photorec`)
- `smartmontools` (provides `smartctl`)
- `hdparm`, `eject`, `notify-send`, `paplay`

### Windows

Terminal-native workflow is available through `cardnuke.py` using built-in Windows CLI tooling plus Python:

- `powershell`, `diskpart`, `chkdsk`, `mountvol`
- Run from an elevated terminal (`Run as administrator`)
- Device targets are disk numbers like `2` or raw paths like `\\.\PhysicalDrive2`

Filesystem support in Windows native mode:

- `fat32`
- `exfat`
- `ntfs`

`ext4` remains Linux-only because Windows does not provide native `ext4` formatting tools.

## Running

```bash
# Interactive
sudo python3 cardnuke.py

# Direct device target
sudo python3 cardnuke.py /dev/sdb

# Lightweight shell variant
sudo bash cardnuke.sh /dev/sdb
```

Windows:

```powershell
# Interactive
python cardnuke.py

# Direct disk target
python cardnuke.py 2

# Or by raw device path
python cardnuke.py \\.\PhysicalDrive2
```

## `cardnuke.py` modes

1. `format`  - wipe/reformat
2. `recover` - recover files (PhotoRec)
3. `health`  - bad block + S.M.A.R.T. check
4. `backup`  - save card to `.img`
5. `speed`   - benchmark write/read
6. `info`    - show card and partition info
7. `repair`  - try filesystem repair
8. `restore` - flash `.img` back to device

### Format levels

- `low`      - reformat only
- `medium`   - zero fill + reformat
- `high`     - 3x random + zero fill + reformat
- `override` - clear read-only flags + high + reformat

## Typical workflows

### Reformat with confidence

1. Launch `cardnuke.py`
2. Choose `format`
3. Optional pre-format health check / backup
4. Confirm with `YES`
5. Select wipe level + filesystem
6. Let verify and speed checks run

### Recover from damaged media

1. Choose `recover`
2. Set output folder
3. Follow PhotoRec prompts
4. Let tool auto-deduplicate and organize recovered files

### Backup before risky changes

1. Choose `backup`
2. Save `.img` and `.sha256`
3. Use `restore` to reflash later if needed

## Logs

Each run creates a timestamped log in home:

- `~/cardnuke_YYYYMMDD_HHMMSS.log`

## Notes

- Use whole-device nodes (like `/dev/sdb`) for format/restore paths.
- NVMe/MMC partition naming is handled automatically on Linux.
- Windows raw-disk operations can be slow on large media because backup, restore, and overwrite passes stream through the device directly from the terminal workflow.
- Recovery quality depends on overwrite history and physical card health.
