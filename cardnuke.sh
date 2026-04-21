#!/usr/bin/env bash
set -e

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0 [device]"
  exit 1
fi

# Pick device
if [[ -n "$1" ]]; then
  DEV="$1"
else
  echo "Available block devices:"
  lsblk -dpno NAME,SIZE,MODEL | grep -v "loop"
  read -rp "Enter device (e.g. /dev/sdb): " DEV
fi

# Confirm
echo ""
lsblk "$DEV"
echo ""
read -rp "⚠️  This will DESTROY all data on $DEV. Type YES to continue: " CONFIRM
[[ "$CONFIRM" != "YES" ]] && echo "Aborted." && exit 0

# Format level
echo ""
echo "Format level:"
echo "  [1] low      - reformat only"
echo "  [2] medium   - zero fill + reformat"
echo "  [3] high     - 3x random fill + zero fill + reformat"
echo "  [4] override - clear RO + high + reformat"
read -rp "Choose [1-4] (default: 2): " LEVEL
LEVEL="${LEVEL:-2}"

# Filesystem choice
read -rp "Filesystem [fat32/exfat/ext4] (default: fat32): " FS
FS="${FS:-fat32}"

# Partition naming
PART="${DEV}1"
[[ "$DEV" =~ [0-9]$ ]] && PART="${DEV}p1"

clear_ro() {
  echo "→ Clearing read-only flags..."
  blockdev --setrw "$DEV" 2>/dev/null && echo "  blockdev: RW set" || echo "  blockdev: skipped"
  echo 0 > /sys/block/$(basename "$DEV")/ro 2>/dev/null && echo "  sysfs ro: cleared" || echo "  sysfs: skipped"
  hdparm -r0 "$DEV" 2>/dev/null && echo "  hdparm: RO cleared" || echo "  hdparm: skipped"
}

do_format() {
  echo "→ Writing new partition table..."
  parted -s "$DEV" mklabel msdos
  parted -s "$DEV" mkpart primary 1MiB 100%
  partprobe "$DEV" || true
  for _ in {1..10}; do
    [[ -b "$PART" ]] && break
    sleep 1
  done
  [[ -b "$PART" ]] || { echo "Partition device did not appear: $PART"; exit 1; }
  echo "→ Formatting $PART as $FS..."
  case "$FS" in
    fat32)  mkfs.vfat -F 32 "$PART" ;;
    exfat)  mkfs.exfat "$PART" ;;
    ext4)   mkfs.ext4 -F "$PART" ;;
    *)      echo "Unknown filesystem: $FS"; exit 1 ;;
  esac
}

echo ""
case "$LEVEL" in
  1)
    do_format
    ;;
  2)
    echo "→ Zero fill..."
    dd if=/dev/zero of="$DEV" bs=4M status=progress conv=fsync
    do_format
    ;;
  3)
    for i in 1 2 3; do
      echo "→ Random pass $i/3..."
      dd if=/dev/urandom of="$DEV" bs=4M status=progress conv=fsync
    done
    echo "→ Zero fill..."
    dd if=/dev/zero of="$DEV" bs=4M status=progress conv=fsync
    do_format
    ;;
  4)
    clear_ro
    for i in 1 2 3; do
      echo "→ Random pass $i/3..."
      dd if=/dev/urandom of="$DEV" bs=4M status=progress conv=fsync
    done
    echo "→ Zero fill..."
    dd if=/dev/zero of="$DEV" bs=4M status=progress conv=fsync
    do_format
    ;;
  *)
    echo "Invalid option."; exit 1 ;;
esac

echo ""
echo "✓ Done. $DEV has been nuked and formatted as $FS."
