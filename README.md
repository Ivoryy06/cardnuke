# cardnuke

> *Wipe, recover, check, back up and restore SD cards and USB drives from one terminal.*

A small Linux tool for the card maintenance photographers end up doing: reformatting a card the way the camera likes it, backing it up before a shoot, checking a card you no longer trust, and getting files back after a mistake.

## Safe by default

A tool that can erase drives should be hard to misuse, so:

- **Nothing destructive runs until you type the device path back.** `format`, `restore`, `repair` and the destructive options all stop and ask. `--yes` skips the question for scripts.
- **The speed and health checks leave your data alone.** They read from the card. The speed test measures writing through a temporary file on the card (and deletes it). Raw writing and the read-write bad block scan only run with `--destructive`, and ask first.
- **It won't touch your running system.** It refuses any disk that holds `/`, `/boot`, `/home`, `/usr` or `/var`, and refuses disks that don't look removable unless you add `--i-know`.
- **It unmounts the card for you** before anything that writes, and stops if it can't.
- **No shell is involved.** A strange device name or label can't run commands.
- **`--dry-run`** shows exactly what would run and changes nothing.

## Install

You need a C++17 compiler (`g++`) and `make`.

```bash
make
sudo make install      # optional: copies it to /usr/local/bin
```

It shells out to standard tools for the heavy lifting. Install the ones for the modes you use:

| Mode | Needs |
|------|-------|
| format | `parted`, `mkfs.vfat` (dosfstools), `mkfs.exfat` (exfatprogs), `mkfs.ext4` (e2fsprogs) |
| health | `badblocks` (e2fsprogs) |
| backup, restore | `dd`, `sha256sum` (coreutils) |
| repair | `fsck.vfat`, `fsck.exfat`, `e2fsck` |
| recover | `photorec` (testdisk) |
| eject | `eject` |

## Use

```bash
sudo ./cardnuke list                          # see the disks
sudo ./cardnuke /dev/sdb info
sudo ./cardnuke /dev/sdb speed                # safe
sudo ./cardnuke /dev/sdb health               # read-only scan
sudo ./cardnuke /dev/sdb backup card.img      # also writes card.img.sha256
sudo ./cardnuke /dev/sdb format exfat MYCARD  # asks first
sudo ./cardnuke /dev/sdb restore card.img     # checks the checksum, then asks
sudo ./cardnuke /dev/sdb eject
sudo ./cardnuke                               # interactive menu
```

Give it the **whole device** (`/dev/sdb`, `/dev/mmcblk0`), not a partition like `/dev/sdb1`.

### Modes

| # | Mode | Changes data? | What it does |
|---|------|---------------|--------------|
| 1 | `format` | yes | Clears the partition table, makes one partition, formats it (vfat, exfat or ext4) |
| 2 | `recover` | no | Starts PhotoRec on the card |
| 3 | `health` | no (yes with `--destructive`) | Bad block scan |
| 4 | `backup` | no | Saves the card to a `.img` with a SHA-256 checksum |
| 5 | `speed` | no (yes with `--destructive`) | Read and write benchmark |
| 6 | `info` | no | Shows card details |
| 7 | `repair` | yes | Runs the right filesystem checker for each partition |
| 8 | `restore` | yes | Writes a `.img` back, after checking size and checksum |
| 9 | `eject` | no | Unmounts and ejects |

### Options

`--dry-run`, `--yes`, `--destructive`, `--force` (overwrite an existing backup), `--allow-file` (work on a disk image file, used by the tests), `--i-know` (allow a disk that doesn't look removable).

## Tests

```bash
make test
```

The tests run against disk image files, so no real card is touched. They check that a speed test and a health scan leave a card byte-for-byte unchanged, that destructive actions refuse without confirmation, that shell commands hidden in a device name or label never run, and that backup and restore round-trip correctly (including refusing a damaged or oversized image).

**What the tests don't cover:** the parts that need real hardware or a mounted card, namely partitioning and formatting, repair, eject, PhotoRec, and the mounted-card write test. Try those on a card you don't care about first.

## Other versions in this repository

`cardnuke_win.cpp` (Windows), `cardnuke.py` and `cardnuke.sh` are older. They haven't been through the safety changes above and haven't been tested. In particular, don't run an older version's speed test on a card that holds files you want to keep.

## License

Use it at your own risk, and always keep a backup of anything that matters.
