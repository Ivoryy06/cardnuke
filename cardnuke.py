#!/usr/bin/env python3
import ctypes
import datetime
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import time

WIN = platform.system() == "Windows"
WINDOWS_FILESYSTEMS = {"fat32", "exfat", "ntfs"}
CHUNK_SIZE = 4 * 1024 * 1024


def log(msg, logfile=None):
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line)
    if logfile:
        with open(logfile, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def run(cmd, *, input_text=None, check=False, capture=True):
    return subprocess.run(
        cmd,
        input=input_text,
        text=True,
        check=check,
        capture_output=capture,
    )


def powershell(script, *, check=False):
    return run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        check=check,
    )


def require_success(result, action, lf):
    if result.returncode == 0:
        return result
    details = ((result.stdout or "") + (result.stderr or "")).strip()
    message = action if not details else f"{action}: {details}"
    log(f"  {message}", lf)
    raise RuntimeError(message)


def root_block_name(dev):
    name = os.path.basename(dev)
    if re.match(r"^(mmcblk\d+|nvme\d+n\d+)p\d+$", name):
        return re.sub(r"p\d+$", "", name)
    return re.sub(r"\d+$", "", name)


def linux_partition_path(dev, index=1):
    return f"{dev}p{index}" if dev[-1].isdigit() else f"{dev}{index}"


def is_admin():
    if WIN:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    return os.geteuid() == 0


def normalize_windows_disk(dev):
    raw = dev.strip().strip('"').strip("'")
    if not raw:
        raise ValueError("device is empty")
    if raw.isdigit():
        num = int(raw)
    else:
        m = re.fullmatch(r"(?:\\\\\.\\)?physicaldrive(\d+)", raw, re.IGNORECASE)
        if not m:
            raise ValueError("use a disk number like 2 or \\\\.\\PhysicalDrive2")
        num = int(m.group(1))
    return {"number": num, "path": rf"\\.\PhysicalDrive{num}"}


def diskpart(script, *, check=False):
    normalized = "\n".join(line.strip() for line in script.strip().splitlines() if line.strip()) + "\n"
    return run(["diskpart"], input_text=normalized, check=check)


def windows_partition_letters(number):
    ps = (
        f"Get-Partition -DiskNumber {number} -ErrorAction SilentlyContinue | "
        "Where-Object DriveLetter | "
        "ForEach-Object { "
        '  "{0}|{1}|{2}" -f $_.PartitionNumber, $_.DriveLetter, $_.Size '
        "}"
    )
    r = powershell(ps)
    entries = []
    for line in r.stdout.splitlines():
        parts = [part.strip() for part in line.split("|")]
        if len(parts) != 3 or not parts[1]:
            continue
        entries.append(
            {
                "partition": int(parts[0]),
                "letter": parts[1].upper(),
                "size": int(parts[2]) if parts[2].isdigit() else 0,
            }
        )
    return entries


def choose_windows_volume(number, lf=None, purpose="use"):
    entries = windows_partition_letters(number)
    if len(entries) == 1:
        return entries[0]["letter"] + ":"
    if len(entries) > 1:
        log(f"  Multiple mounted volumes detected for disk {number}.", lf)
        for idx, entry in enumerate(entries, start=1):
            size_gb = entry["size"] / 1024**3 if entry["size"] else 0
            print(f"  [{idx}] Partition {entry['partition']}  {entry['letter']}:  {size_gb:.2f} GB")
        choice = input(f"Choose volume to {purpose} [1-{len(entries)}] (default 1): ").strip() or "1"
        if choice.isdigit() and 1 <= int(choice) <= len(entries):
            return entries[int(choice) - 1]["letter"] + ":"
        log("  Invalid volume selection.", lf)
        return None
    log("  No mounted volume letter found.", lf)
    return None


def windows_prepare_disk(number, lf):
    script = f"""
    Set-Disk -Number {number} -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number {number} -IsReadOnly $false -ErrorAction SilentlyContinue
    Update-HostStorageCache
    """
    require_success(powershell(script), f"Unable to prepare disk {number}", lf)
    log(f"  Disk {number} is online and writable.", lf)


def windows_refresh_storage():
    powershell("Update-HostStorageCache")


def windows_set_disk_offline(number, offline, lf):
    state = "$true" if offline else "$false"
    label = "offline" if offline else "online"
    script = f"""
    Set-Disk -Number {number} -IsReadOnly $false -ErrorAction SilentlyContinue
    Set-Disk -Number {number} -IsOffline {state} -ErrorAction Stop
    Update-HostStorageCache
    """
    require_success(powershell(script), f"Unable to set disk {number} {label}", lf)
    log(f"  Disk {number} set {label}.", lf)


def windows_dismount_volumes(number, lf):
    script = f"""
    $letters = Get-Partition -DiskNumber {number} -ErrorAction SilentlyContinue |
      Where-Object DriveLetter |
      Select-Object -ExpandProperty DriveLetter
    foreach ($letter in $letters) {{
      mountvol "$($letter):" /p
    }}
    """
    require_success(powershell(script), f"Unable to dismount volumes on disk {number}", lf)
    log("  Requested volume dismount.", lf)


def windows_with_raw_disk(number, lf, action):
    windows_prepare_disk(number, lf)
    windows_dismount_volumes(number, lf)
    windows_set_disk_offline(number, True, lf)
    try:
        return action()
    finally:
        try:
            windows_set_disk_offline(number, False, lf)
        except RuntimeError:
            log(f"  Warning: disk {number} may still be offline; bring it online manually if needed.", lf)


def stream_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def physical_drive_size(path):
    if not WIN:
        return os.path.getsize(path)
    fh = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    try:
        return os.lseek(fh, 0, os.SEEK_END)
    finally:
        os.close(fh)


def write_pattern_windows(path, pattern, total_bytes, lf):
    flags = os.O_WRONLY | getattr(os, "O_BINARY", 0)
    fd = os.open(path, flags)
    written = 0
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        while written < total_bytes:
            chunk = pattern(min(CHUNK_SIZE, total_bytes - written))
            os.write(fd, chunk)
            written += len(chunk)
            pct = written * 100 / total_bytes
            print(f"\r  {pct:5.1f}% ({written // (1024 * 1024)} MiB/{total_bytes // (1024 * 1024)} MiB)", end="", flush=True)
        print()
    finally:
        os.close(fd)
    log("  Write pass complete.", lf)


def copy_file_to_device_windows(src, dst, lf):
    size = os.path.getsize(src)
    flags = os.O_WRONLY | getattr(os, "O_BINARY", 0)
    out_fd = os.open(dst, flags)
    copied = 0
    try:
        os.lseek(out_fd, 0, os.SEEK_SET)
        with open(src, "rb") as f:
            for chunk in iter(lambda: f.read(CHUNK_SIZE), b""):
                os.write(out_fd, chunk)
                copied += len(chunk)
                pct = copied * 100 / size
                print(f"\r  {pct:5.1f}% ({copied // (1024 * 1024)} MiB/{size // (1024 * 1024)} MiB)", end="", flush=True)
        print()
    finally:
        os.close(out_fd)
    log("  Raw restore write complete.", lf)


def copy_device_to_file_windows(src, dst, lf):
    total = physical_drive_size(src)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
    in_fd = os.open(src, flags)
    copied = 0
    try:
        os.lseek(in_fd, 0, os.SEEK_SET)
        with open(dst, "wb") as f:
            while True:
                chunk = os.read(in_fd, CHUNK_SIZE)
                if not chunk:
                    break
                f.write(chunk)
                copied += len(chunk)
                pct = copied * 100 / total if total else 0
                print(f"\r  {pct:5.1f}% ({copied // (1024 * 1024)} MiB/{total // (1024 * 1024)} MiB)", end="", flush=True)
        print()
    finally:
        os.close(in_fd)
    log("  Raw backup read complete.", lf)


def verify_image_against_device_windows(img, dev_path, lf):
    expected = stream_sha256(img)
    remaining = os.path.getsize(img)
    digest = hashlib.sha256()
    fd = os.open(dev_path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
    read_total = 0
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        while remaining > 0:
            chunk = os.read(fd, min(CHUNK_SIZE, remaining))
            if not chunk:
                break
            digest.update(chunk)
            remaining -= len(chunk)
            read_total += len(chunk)
            pct = read_total * 100 / os.path.getsize(img)
            print(f"\r  {pct:5.1f}% verify", end="", flush=True)
        print()
    finally:
        os.close(fd)
    actual = digest.hexdigest()
    log(f"  {'✓ Match' if actual == expected else '✗ MISMATCH — restore may be corrupt'}", lf)


def ask_windows_filesystem(lf):
    fs = input("Filesystem [fat32/exfat/ntfs] (default fat32): ").strip().lower() or "fat32"
    if fs not in WINDOWS_FILESYSTEMS:
        log("  Windows native mode supports fat32, exfat, and ntfs only.", lf)
        return None
    return fs


def list_devices():
    if WIN:
        script = r"""
        Get-Disk |
        Sort-Object Number |
        ForEach-Object {
          $size = if ($_.Size) { "{0:N2} GB" -f ($_.Size / 1GB) } else { "0 GB" }
          $state = if ($_.IsReadOnly) { "RO" } else { "RW" }
          "{0,-6} {1,-12} {2,-12} {3,-10} {4}" -f $_.Number, $size, $_.BusType, $state, $_.FriendlyName
        }
        """
        r = powershell(script)
        print(f"{'DISK':<6} {'SIZE':<9} {'BUS':<12} {'STATE':<10} MODEL")
        print("-" * 70)
        print(r.stdout.strip())
        print("\nUse the disk number above or \\\\.\\PhysicalDriveN.")
        return

    result = run(["lsblk", "-dpno", "NAME,SIZE,MODEL,TRAN,RM"])
    print(f"{'DEVICE':<12} {'SIZE':<10} {'MODEL':<30} {'TRAN':<8} {'REMOVABLE'}")
    print("-" * 70)
    for line in result.stdout.strip().splitlines():
        parts = line.split()
        removable = parts[-1] == "1" if parts else False
        print(line + (" ◄ SD/USB" if removable else ""))


def eject(dev, lf):
    if input(f"\nEject {dev} now? [y/N]: ").strip().lower() != "y":
        return
    log(f"→ Ejecting {dev}...", lf)
    if WIN:
        number = normalize_windows_disk(dev)["number"]
        ps = f"""
        $letters = Get-Partition -DiskNumber {number} -ErrorAction SilentlyContinue |
          Where-Object DriveLetter |
          Select-Object -ExpandProperty DriveLetter
        foreach ($letter in $letters) {{
          mountvol "$($letter):" /p
        }}
        """
        powershell(ps)
        log("  Volumes dismounted.", lf)
        return

    run(["umount", "-l", dev])
    r = run(["eject", dev])
    if r.returncode == 0:
        log("  ✓ Ejected. Safe to remove.", lf)
    else:
        log("  Unmounted (eject cmd not available — safe to remove).", lf)


def notify(msg):
    if WIN:
        powershell(
            "Add-Type -AssemblyName System.Windows.Forms;"
            f'[System.Windows.Forms.MessageBox]::Show("{msg}","cardnuke") | Out-Null'
        )
        return
    if shutil.which("notify-send"):
        run(["notify-send", "cardnuke", msg])
    if shutil.which("paplay"):
        run(["paplay", "/usr/share/sounds/freedesktop/stereo/complete.oga"])


def clear_ro(dev, lf):
    log("→ Clearing read-only flags...", lf)
    if WIN:
        number = normalize_windows_disk(dev)["number"]
        windows_prepare_disk(number, lf)
        require_success(diskpart(f"""
        select disk {number}
        attributes disk clear readonly
        online disk noerr
        """), f"Unable to clear readonly flag on disk {number}", lf)
        log("  diskpart: readonly cleared", lf)
    else:
        run(["blockdev", "--setrw", dev])
        try:
            with open(f"/sys/block/{root_block_name(dev)}/ro", "w", encoding="utf-8") as f:
                f.write("0")
            log("  sysfs ro: cleared", lf)
        except Exception:
            log("  sysfs: skipped", lf)
        run(["hdparm", "-r0", dev])
    log("  done.", lf)


def health_check(dev, lf):
    log("→ Running health / bad block check...", lf)
    if WIN:
        number = normalize_windows_disk(dev)["number"]
        volume = choose_windows_volume(number, lf, purpose="scan")
        if volume:
            r = run(["chkdsk", volume, "/scan"])
            text = (r.stdout or "") + (r.stderr or "")
            for line in text.splitlines():
                if line.strip():
                    log(f"  {line.strip()}", lf)
        else:
            log("  No mounted volume to scan. Format or assign a drive letter first.", lf)
        return
    if shutil.which("smartctl"):
        r = run(["smartctl", "-H", dev])
        for line in r.stdout.splitlines():
            if "SMART" in line or "result" in line.lower():
                log(f"  smartctl: {line.strip()}", lf)
    else:
        log("  smartctl not found (install smartmontools for S.M.A.R.T.)", lf)
    log("  Running badblocks read-only scan (this may take a while)...", lf)
    r = run(["badblocks", "-sv", dev])
    bad = r.stdout.strip() or r.stderr.strip()
    log(f"  badblocks: {bad if bad else 'No bad blocks found.'}", lf)


def backup(dev, lf):
    default = os.path.expanduser(f"~/cardnuke_backup_{os.path.basename(dev)}_{int(time.time())}.img")
    out = input(f"Backup image path (default: {default}): ").strip() or default
    log(f"→ Backing up {dev} to {out}...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        windows_with_raw_disk(info["number"], lf, lambda: copy_device_to_file_windows(info["path"], out, lf))
    else:
        subprocess.run(["dd", f"if={dev}", f"of={out}", "bs=4M", "status=progress", "conv=fsync"], check=True)
    digest = stream_sha256(out)
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write(digest + "\n")
    log(f"  SHA256: {digest} → {out}.sha256", lf)


def dd_fill(dev, source, label, lf):
    log(f"→ {label}...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        def do_write():
            total = physical_drive_size(info["path"])
            if source == "random":
                write_pattern_windows(info["path"], os.urandom, total, lf)
            else:
                zero_block = b"\x00" * CHUNK_SIZE
                write_pattern_windows(info["path"], lambda size: zero_block[:size], total, lf)
        windows_with_raw_disk(info["number"], lf, do_write)
        windows_refresh_storage()
        return
    src = "/dev/urandom" if source == "random" else "/dev/zero"
    subprocess.run(["dd", f"if={src}", f"of={dev}", "bs=4M", "status=progress", "conv=fsync"], check=True)


def wait_for_linux_partition(dev, part, attempts=10, delay=1.0):
    for _ in range(attempts):
        if os.path.exists(part):
            return
        subprocess.run(["partprobe", dev], capture_output=True)
        time.sleep(delay)
    raise RuntimeError(f"Partition device did not appear: {part}")


def ensure_windows_format_supported(number, fs, lf):
    if fs != "fat32":
        return
    result = require_success(
        powershell(f"(Get-Disk -Number {number} -ErrorAction Stop).Size"),
        f"Unable to read disk size for {number}",
        lf,
    )
    try:
        size_bytes = int(result.stdout.strip())
    except ValueError as e:
        raise RuntimeError(f"Unable to parse disk size for {number}") from e
    if size_bytes > 32 * 1024**3:
        raise RuntimeError(
            "Windows native FAT32 formatting is limited to disks up to 32 GB; use exfat/ntfs or format FAT32 on Linux"
        )


def do_format(dev, fs, label, lf):
    log(f"→ Formatting as {fs}" + (f" (label: {label})" if label else "") + "...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        if fs not in WINDOWS_FILESYSTEMS:
            log("  Windows native mode supports fat32, exfat, and ntfs only.", lf)
            return False
        windows_prepare_disk(info["number"], lf)
        ensure_windows_format_supported(info["number"], fs, lf)
        label_arg = f'label="{label}"' if label else ""
        require_success(diskpart(f"""
        select disk {info["number"]}
        online disk noerr
        attributes disk clear readonly noerr
        clean
        convert mbr
        create partition primary
        format fs={fs} {label_arg} quick
        assign
        """), f"diskpart format failed for disk {info['number']}", lf)
        windows_refresh_storage()
        return True

    part = linux_partition_path(dev)
    subprocess.run(["parted", "-s", dev, "mklabel", "msdos"], check=True)
    subprocess.run(["parted", "-s", dev, "mkpart", "primary", "1MiB", "100%"], check=True)
    subprocess.run(["partprobe", dev], capture_output=True)
    wait_for_linux_partition(dev, part)
    if fs == "fat32":
        cmd = ["mkfs.vfat", "-F", "32"]
        if label:
            cmd += ["-n", label[:11].upper()]
    elif fs == "exfat":
        cmd = ["mkfs.exfat"]
        if label:
            cmd += ["-L", label]
    else:
        cmd = ["mkfs.ext4", "-F"]
        if label:
            cmd += ["-L", label]
    cmd.append(part)
    subprocess.run(cmd, check=True)
    return True


def verify_write(dev, lf):
    log("→ Verifying write integrity...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        volume = choose_windows_volume(info["number"], lf, purpose="verify")
        if not volume:
            return
        tf = os.path.join(volume + "\\", ".cardnuke_test")
        data = os.urandom(1024 * 1024)
        try:
            with open(tf, "wb") as f:
                f.write(data)
            with open(tf, "rb") as f:
                ok = f.read() == data
            os.remove(tf)
            log(f"  {'✓ passed' if ok else '✗ FAILED — media may be faulty'}", lf)
        except Exception as e:
            log(f"  Verify error: {e}", lf)
        return

    part = linux_partition_path(dev)
    mnt = "/tmp/cardnuke_verify"
    os.makedirs(mnt, exist_ok=True)
    try:
        subprocess.run(["mount", part, mnt], check=True)
        data = os.urandom(1024 * 1024)
        tf = os.path.join(mnt, ".cardnuke_test")
        with open(tf, "wb") as f:
            f.write(data)
        with open(tf, "rb") as f:
            ok = f.read() == data
        os.remove(tf)
        log(f"  {'✓ passed' if ok else '✗ FAILED — card may be faulty'}", lf)
    except Exception as e:
        log(f"  Verify error: {e}", lf)
    finally:
        subprocess.run(["umount", mnt], capture_output=True)
        os.rmdir(mnt)


def speed_test(dev, lf):
    log("→ Speed test...", lf)
    if WIN:
        log("  Speed test not supported on Windows yet", lf)
        return

    mnt = "/tmp/cardnuke_speed"
    os.makedirs(mnt, exist_ok=True)

    if os.path.ismount(mnt):
        subprocess.run(["umount", mnt], capture_output=True)

    try:
        dev_path = linux_partition_path(dev)
        if not os.path.exists(dev_path):
            dev_path = dev
        r = subprocess.run(["mount", "-o", "sync", dev_path, mnt], capture_output=True)
        if r.returncode != 0:
            log(f"  Mount error: {r.stderr.decode()}", lf)
            return

        binpath = os.path.join(os.path.dirname(os.path.abspath(__file__)), "speed_test")
        r = subprocess.run([binpath, mnt, "both"], capture_output=True, text=True)
        if r.returncode != 0:
            log(f"  Error: {r.stderr}", lf)
            return
        if r.stderr:
            for line in r.stderr.strip().split("\n"):
                log(f"  {line}", lf)
        parts = r.stdout.strip().split()
        if len(parts) == 2:
            log(f"  Write: {parts[0]} MB/s  Read: {parts[1]} MB/s", lf)
    except Exception as e:
        log(f"  Speed test error: {e}", lf)
    finally:
        subprocess.run(["umount", mnt], capture_output=True)
        os.rmdir(mnt)


def card_info(dev, lf):
    log(f"→ Card info for {dev}...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        r = powershell(
            f"Get-Disk -Number {info['number']} | "
            "Format-List Number,FriendlyName,SerialNumber,BusType,PartitionStyle,Size,HealthStatus,OperationalStatus,IsBoot,IsSystem,IsReadOnly"
        )
        print(r.stdout)
        r = powershell(
            f"Get-Partition -DiskNumber {info['number']} -ErrorAction SilentlyContinue | "
            "Format-Table PartitionNumber,DriveLetter,Size,Type -AutoSize"
        )
        print(r.stdout)
        return

    subprocess.run(["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL", dev])
    subprocess.run(["parted", "-s", dev, "print"], capture_output=False)
    r = run(["df", "-h"])
    for line in r.stdout.splitlines():
        if dev in line:
            log(f"  usage: {line.strip()}", lf)


def repair(dev, lf):
    log(f"→ Repairing filesystem on {dev}...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        volume = choose_windows_volume(info["number"], lf, purpose="repair")
        if not volume:
            log("  No mounted volume found to repair.", lf)
            return
        r = run(["chkdsk", volume, "/f"])
        text = (r.stdout or "") + (r.stderr or "")
        for line in text.splitlines():
            if line.strip():
                log(f"  {line.strip()}", lf)
        return

    part = linux_partition_path(dev)
    subprocess.run(["umount", part], capture_output=True)
    r = run(["blkid", "-o", "value", "-s", "TYPE", part])
    fstype = r.stdout.strip()
    if not fstype:
        log("  Cannot detect filesystem — try format instead.", lf)
        return
    log(f"  Detected: {fstype}", lf)
    if fstype in ("vfat", "fat32", "fat16"):
        subprocess.run(["fsck.vfat", "-a", part])
    elif fstype == "exfat":
        subprocess.run(["fsck.exfat", "-y", part])
    elif fstype.startswith("ext"):
        subprocess.run(["fsck", "-y", part])
    else:
        log(f"  No fsck available for {fstype}.", lf)


def restore(dev, lf):
    img = input("Path to .img file to restore: ").strip()
    if not os.path.isfile(img):
        log(f"  File not found: {img}", lf)
        return
    confirm = input(f"Overwriting ALL data on {dev} with {img}. Type YES: ")
    if confirm != "YES":
        log("  Aborted.", lf)
        return
    log(f"→ Restoring {img} → {dev}...", lf)
    if WIN:
        info = normalize_windows_disk(dev)
        windows_with_raw_disk(info["number"], lf, lambda: copy_file_to_device_windows(img, info["path"], lf))
        windows_refresh_storage()
        sha_file = img + ".sha256"
        if os.path.isfile(sha_file):
            log("→ Verifying restore checksum...", lf)
            windows_with_raw_disk(info["number"], lf, lambda: verify_image_against_device_windows(img, info["path"], lf))
        log("  ✓ Restore complete.", lf)
        return

    subprocess.run(["dd", f"if={img}", f"of={dev}", "bs=4M", "status=progress", "conv=fsync"], check=True)
    sha_file = img + ".sha256"
    if os.path.isfile(sha_file):
        expected = open(sha_file, encoding="utf-8").read().strip()
        log("→ Verifying restore checksum...", lf)
        img_size = os.path.getsize(img)
        sha = hashlib.sha256()
        remaining = img_size
        with open(dev, "rb") as f:
            while remaining > 0:
                chunk = f.read(min(1024 * 1024, remaining))
                if not chunk:
                    break
                sha.update(chunk)
                remaining -= len(chunk)
        actual = sha.hexdigest()
        log(f"  {'✓ Match' if actual == expected else '✗ MISMATCH — restore may be corrupt'}", lf)
    subprocess.run(["partprobe", dev], capture_output=True)
    log("  ✓ Restore complete.", lf)


def organize(folder, lf):
    log("→ Organizing recovered files by type...", lf)
    type_map = {
        "photos": {"jpg", "jpeg", "png", "gif", "bmp", "tiff", "raw", "cr2", "nef", "heic"},
        "videos": {"mp4", "mov", "avi", "mkv", "3gp", "wmv"},
        "audio": {"mp3", "wav", "aac", "flac", "ogg", "m4a"},
        "documents": {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "txt"},
        "archives": {"zip", "rar", "7z", "tar", "gz"},
    }
    moved = 0
    for root, _, files in os.walk(folder):
        if any(os.path.basename(root) == cat for cat in type_map):
            continue
        for fname in files:
            ext = fname.rsplit(".", 1)[-1].lower() if "." in fname else ""
            dest_cat = next((cat for cat, exts in type_map.items() if ext in exts), "other")
            dest_dir = os.path.join(folder, dest_cat)
            os.makedirs(dest_dir, exist_ok=True)
            src = os.path.join(root, fname)
            dst = os.path.join(dest_dir, fname)
            if os.path.exists(dst):
                base, ext2 = os.path.splitext(fname)
                dst = os.path.join(dest_dir, f"{base}_{int(time.time())}{ext2}")
            shutil.move(src, dst)
            moved += 1
    log(f"  Organized {moved} file(s) into subfolders.", lf)


def dedup(folder, lf):
    log("→ Removing duplicate files...", lf)
    seen = {}
    removed = 0
    for root, _, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(root, fname)
            try:
                digest = hashlib.md5()
                with open(fpath, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                        digest.update(chunk)
                file_hash = digest.hexdigest()
                if file_hash in seen:
                    os.remove(fpath)
                    removed += 1
                else:
                    seen[file_hash] = fpath
            except Exception:
                pass
    log(f"  Removed {removed} duplicate(s).", lf)


FILE_TYPES = {
    "1": ("Photos", ["jpg", "png", "gif", "bmp", "tiff", "raw", "cr2", "nef"]),
    "2": ("Videos", ["mp4", "mov", "avi", "mkv", "3gp", "wmv"]),
    "3": ("Audio", ["mp3", "wav", "aac", "flac", "ogg", "m4a"]),
    "4": ("Documents", ["pdf", "doc", "docx", "xls", "xlsx", "txt"]),
    "5": ("Archives", ["zip", "rar", "7z", "tar", "gz"]),
    "6": ("All", []),
}

EXT_FAMILY = {
    "jpg": "jpg",
    "jpeg": "jpg",
    "png": "png",
    "gif": "gif",
    "bmp": "bmp",
    "tiff": "tiff",
    "raw": "raw",
    "cr2": "raw",
    "nef": "raw",
    "mp4": "mp4",
    "mov": "mov",
    "avi": "avi",
    "mkv": "mkv",
    "3gp": "3gp",
    "wmv": "wmv",
    "mp3": "mp3",
    "wav": "wav",
    "aac": "aac",
    "flac": "flac",
    "ogg": "ogg",
    "m4a": "mp4",
    "pdf": "pdf",
    "doc": "doc",
    "docx": "doc",
    "xls": "xls",
    "xlsx": "xls",
    "txt": "txt",
    "zip": "zip",
    "rar": "rar",
    "7z": "7z",
    "tar": "tar",
    "gz": "gz",
}


def pick_file_types(lf):
    print("\nFile type filter:")
    for key, (label, exts) in FILE_TYPES.items():
        print(f"  [{key}] {label:<12} ({', '.join(exts) if exts else 'everything'})")
    choices = input("Choose types (e.g. 1 2, default 6=All): ").strip() or "6"
    exts = []
    for choice in choices.split():
        if choice in FILE_TYPES:
            exts.extend(FILE_TYPES[choice][1])
    if not exts:
        log("  File filter: All types", lf)
        return None
    log(f"  File filter: {', '.join(exts)}", lf)
    return exts


def write_photorec_cfg(exts, out_dir):
    families = sorted(set(EXT_FAMILY[e] for e in exts if e in EXT_FAMILY))
    cfg = os.path.join(out_dir, "photorec.cfg")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("[photorec]\nparanoid=1\nkeep_corrupted_file=0\ninterface=ncurses\n")
        f.write(f"file_opt={','.join(families)}\n")


def do_recover(dev, lf):
    tool = "photorec_win.exe" if WIN else "photorec"
    if not shutil.which(tool):
        log("photorec not found. Install testdisk package.", lf)
        sys.exit(1)
    out = input("Output folder (default ~/Downloads/recover): ").strip()
    out = out or os.path.expanduser("~/Downloads/recover")
    os.makedirs(out, exist_ok=True)

    exts = pick_file_types(lf)
    if exts:
        write_photorec_cfg(exts, out)

    print("\nLimit recovered output (leave blank = no limit):")
    max_files = input("  Max files (e.g. 100): ").strip()
    max_size = input("  Max size in GB (e.g. 2.5): ").strip()

    log(f"→ Launching PhotoRec on {dev} → {out}", lf)
    print("  (Follow prompts — select partition then choose the output folder above)\n")
    subprocess.run([tool, dev])

    if max_files or max_size:
        all_files = sorted(
            [os.path.join(root, name) for root, _, files in os.walk(out) for name in files],
            key=os.path.getmtime,
        )
        limit_count = int(max_files) if max_files else None
        limit_bytes = int(float(max_size) * 1024**3) if max_size else None
        kept = total = 0
        for fpath in all_files:
            size = os.path.getsize(fpath)
            if (limit_count and kept >= limit_count) or (limit_bytes and total + size > limit_bytes):
                os.remove(fpath)
            else:
                kept += 1
                total += size
        log(f"  Kept {kept} file(s) ({total / 1024**2:.1f} MB)", lf)

    dedup(out, lf)
    organize(out, lf)
    notify("Recovery complete!")


def main():
    if not is_admin():
        print("Run as administrator/root.")
        sys.exit(1)

    lf = os.path.expanduser(f"~/cardnuke_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
    log("=== cardnuke started ===", lf)

    print("\n=== cardnuke ===\n")
    list_devices()

    example = "disk number or \\\\.\\PhysicalDriveN" if WIN else "/dev/sdb"
    dev = sys.argv[1] if len(sys.argv) > 1 else input(f"\nEnter device ({example}): ").strip()
    if WIN:
        try:
            normalize_windows_disk(dev)
        except ValueError as e:
            log(f"Invalid Windows device: {e}", lf)
            return
    log(f"Device: {dev}", lf)

    print("\nMode:")
    print("  [1] format  - wipe/reformat")
    print("  [2] recover - recover files (PhotoRec)")
    print("  [3] health  - bad block + filesystem scan")
    print("  [4] backup  - image the card to a .img file")
    print("  [5] speed   - benchmark read/write speed")
    print("  [6] info    - show card info")
    print("  [7] repair  - fix corrupted filesystem (no wipe)")
    print("  [8] restore - flash a .img backup back to card")
    mode = input("Choose [1-8] (default 1): ").strip() or "1"
    log(f"Mode: {mode}", lf)

    try:
        if mode == "2":
            do_recover(dev, lf)
            eject(dev, lf)
            return
        if mode == "3":
            health_check(dev, lf)
            return
        if mode == "4":
            backup(dev, lf)
            return
        if mode == "5":
            speed_test(dev, lf)
            return
        if mode == "6":
            card_info(dev, lf)
            return
        if mode == "7":
            repair(dev, lf)
            return
        if mode == "8":
            restore(dev, lf)
            eject(dev, lf)
            return

        if input("\nRun health check before format? [y/N]: ").strip().lower() == "y":
            health_check(dev, lf)
        if input("Backup card before format? [y/N]: ").strip().lower() == "y":
            backup(dev, lf)

        confirm = input(f"\nDESTROY all data on {dev}? Type YES: ")
        if confirm != "YES":
            log("Aborted.", lf)
            return

        print("\nFormat level:")
        print("  [1] low      - reformat only")
        print("  [2] medium   - zero fill + reformat")
        print("  [3] high     - 3x random + zero fill + reformat")
        print("  [4] override - clear RO + high + reformat")
        level = input("Choose [1-4] (default 2): ").strip() or "2"
        if WIN:
            fs = ask_windows_filesystem(lf)
            if not fs:
                return
        else:
            fs = input("Filesystem [fat32/exfat/ext4] (default fat32): ").strip().lower() or "fat32"
        label = input("Volume label (optional, e.g. GOPRO): ").strip()
        log(f"Level: {level}, FS: {fs}, Label: {label or '(none)'}", lf)

        if level == "4":
            clear_ro(dev, lf)
        if level in ("2", "3", "4"):
            if level in ("3", "4"):
                for i in range(1, 4):
                    dd_fill(dev, "random", f"Random pass {i}/3", lf)
            dd_fill(dev, "zero", "Zero fill", lf)

        if not do_format(dev, fs, label, lf):
            return
        verify_write(dev, lf)
        speed_test(dev, lf)

        log(f"✓ Done. {dev} formatted as {fs}.", lf)
        print(f"\nLog saved to: {lf}")
        notify(f"cardnuke done: {dev} formatted as {fs}")
        eject(dev, lf)
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e:
        log(f"ERROR: {e}", lf)
        sys.exit(1)


if __name__ == "__main__":
    main()
