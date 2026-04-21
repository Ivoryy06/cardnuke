#!/usr/bin/env python3
import os, re, sys, subprocess, platform, shutil, hashlib, time, datetime

WIN = platform.system() == "Windows"

def root_block_name(dev):
    """Return the parent block-device name for /sys/block access."""
    name = os.path.basename(dev)
    # /dev/mmcblk0p1 or /dev/nvme0n1p1 -> mmcblk0 / nvme0n1
    if re.match(r"^(mmcblk\d+|nvme\d+n\d+)p\d+$", name):
        return re.sub(r"p\d+$", "", name)
    # /dev/sdb1 -> sdb
    return re.sub(r"\d+$", "", name)

def is_admin():
    if WIN:
        import ctypes
        return ctypes.windll.shell32.IsUserAnAdmin()
    return os.geteuid() == 0

def log(msg, logfile=None):
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line)
    if logfile:
        with open(logfile, "a") as f:
            f.write(line + "\n")

def list_devices():
    if WIN:
        result = subprocess.run(
            ["wmic", "diskdrive", "get", "DeviceID,Size,Model,MediaType"],
            capture_output=True, text=True)
        print(result.stdout)
    else:
        result = subprocess.run(
            ["lsblk", "-dpno", "NAME,SIZE,MODEL,TRAN,RM"],
            capture_output=True, text=True)
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
        subprocess.run(["mountvol", dev, "/p"], capture_output=True)
    else:
        subprocess.run(["umount", "-l", dev], capture_output=True)
        r = subprocess.run(["eject", dev], capture_output=True)
        if r.returncode == 0:
            log("  ✓ Ejected. Safe to remove.", lf)
        else:
            log("  Unmounted (eject cmd not available — safe to remove).", lf)

def notify(msg):
    """Desktop/sound notification when long ops finish."""
    if WIN:
        subprocess.run(["powershell", "-Command",
            f'[System.Reflection.Assembly]::LoadWithPartialName("System.Windows.Forms");'
            f'[System.Windows.Forms.MessageBox]::Show("{msg}","cardnuke")'],
            capture_output=True)
    else:
        if shutil.which("notify-send"):
            subprocess.run(["notify-send", "cardnuke", msg], capture_output=True)
        if shutil.which("paplay"):
            subprocess.run(["paplay", "/usr/share/sounds/freedesktop/stereo/complete.oga"],
                           capture_output=True)

def clear_ro(dev, lf):
    log("→ Clearing read-only flags...", lf)
    if WIN:
        subprocess.run(["diskpart"], input=f"select disk {dev}\nattributes disk clear readonly\n", text=True)
    else:
        subprocess.run(["blockdev", "--setrw", dev], capture_output=True)
        try:
            with open(f"/sys/block/{root_block_name(dev)}/ro", "w") as f:
                f.write("0")
            log("  sysfs ro: cleared", lf)
        except Exception:
            log("  sysfs: skipped", lf)
        subprocess.run(["hdparm", "-r0", dev], capture_output=True)
    log("  done.", lf)

def health_check(dev, lf):
    log("→ Running health / bad block check...", lf)
    if WIN:
        log("  Windows: use chkdsk manually for bad block scan.", lf)
        return
    if shutil.which("smartctl"):
        r = subprocess.run(["smartctl", "-H", dev], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if "SMART" in line or "result" in line.lower():
                log(f"  smartctl: {line.strip()}", lf)
    else:
        log("  smartctl not found (install smartmontools for S.M.A.R.T.)", lf)
    log("  Running badblocks read-only scan (this may take a while)...", lf)
    r = subprocess.run(["badblocks", "-sv", dev], capture_output=True, text=True)
    bad = r.stdout.strip() or r.stderr.strip()
    log(f"  badblocks: {bad if bad else 'No bad blocks found.'}", lf)

def backup(dev, lf):
    default = os.path.expanduser(f"~/cardnuke_backup_{os.path.basename(dev)}_{int(time.time())}.img")
    out = input(f"Backup image path (default: {default}): ").strip() or default
    log(f"→ Backing up {dev} to {out}...", lf)
    if WIN:
        log("  Windows: use Win32DiskImager for backups.", lf)
        return
    subprocess.run(["dd", f"if={dev}", f"of={out}", "bs=4M", "status=progress", "conv=fsync"])
    sha = hashlib.sha256()
    with open(out, "rb") as f:
        for chunk in iter(lambda: f.read(1024*1024), b""):
            sha.update(chunk)
    digest = sha.hexdigest()
    with open(out + ".sha256", "w") as f:
        f.write(digest + "\n")
    log(f"  SHA256: {digest} → {out}.sha256", lf)

def dd_fill(dev, source, label, lf):
    log(f"→ {label}...", lf)
    if WIN:
        subprocess.run(["diskpart"],
            input=f"select disk {dev}\nclean all\n", text=True)
    else:
        src = "/dev/urandom" if source == "random" else "/dev/zero"
        subprocess.run(["dd", f"if={src}", f"of={dev}", "bs=4M", "status=progress", "conv=fsync"])

def do_format(dev, fs, label, lf):
    log(f"→ Formatting as {fs}" + (f" (label: {label})" if label else "") + "...", lf)
    if WIN:
        fs_map = {"fat32": "fat32", "exfat": "exfat", "ext4": "ntfs"}
        fmt = fs_map.get(fs, "fat32")
        label_part = f'label="{label}"' if label else ""
        script = (f"select disk {dev}\nclean\ncreate partition primary\n"
                  f"format fs={fmt} {label_part} quick\nassign\n")
        subprocess.run(["diskpart"], input=script, text=True)
    else:
        part = dev + "1" if not dev[-1].isdigit() else dev + "p1"
        subprocess.run(["parted", "-s", dev, "mklabel", "msdos"])
        subprocess.run(["parted", "-s", dev, "mkpart", "primary", "1MiB", "100%"])
        if fs == "fat32":
            cmd = ["mkfs.vfat", "-F", "32"]
            if label: cmd += ["-n", label[:11].upper()]
            cmd.append(part)
        elif fs == "exfat":
            cmd = ["mkfs.exfat"]
            if label: cmd += ["-L", label]
            cmd.append(part)
        else:
            cmd = ["mkfs.ext4", "-F"]
            if label: cmd += ["-L", label]
            cmd.append(part)
        subprocess.run(cmd)

def verify_write(dev, lf):
    log("→ Verifying write integrity...", lf)
    if WIN:
        log("  Skipped on Windows.", lf)
        return
    part = dev + "1" if not dev[-1].isdigit() else dev + "p1"
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
        log("  Skipped on Windows.", lf)
        return
    part = dev + "1" if not dev[-1].isdigit() else dev + "p1"
    mnt = "/tmp/cardnuke_speed"
    os.makedirs(mnt, exist_ok=True)
    try:
        subprocess.run(["mount", part, mnt], check=True)
        size = 64 * 1024 * 1024
        data = os.urandom(size)
        tf = os.path.join(mnt, ".speed_test")
        t = time.time()
        with open(tf, "wb") as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        w = size / (time.time() - t) / 1024 / 1024
        t = time.time()
        open(tf, "rb").read()
        r = size / (time.time() - t) / 1024 / 1024
        os.remove(tf)
        log(f"  Write: {w:.1f} MB/s  Read: {r:.1f} MB/s", lf)
    except Exception as e:
        log(f"  Speed test error: {e}", lf)
    finally:
        subprocess.run(["umount", mnt], capture_output=True)
        os.rmdir(mnt)

def card_info(dev, lf):
    log(f"→ Card info for {dev}...", lf)
    if WIN:
        subprocess.run(["wmic", "diskdrive", "where", f"DeviceID='{dev}'",
                        "get", "Size,Model,MediaType"])
    else:
        subprocess.run(["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL", dev])
        subprocess.run(["parted", "-s", dev, "print"], capture_output=False)
        r = subprocess.run(["df", "-h"], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if dev in line:
                log(f"  usage: {line.strip()}", lf)

def repair(dev, lf):
    log(f"→ Repairing filesystem on {dev}...", lf)
    if WIN:
        subprocess.run(["chkdsk", dev, "/f"])
        return
    # detect partition
    part = dev + "1" if not dev[-1].isdigit() else dev + "p1"
    # unmount first
    subprocess.run(["umount", part], capture_output=True)
    r = subprocess.run(["blkid", "-o", "value", "-s", "TYPE", part],
                       capture_output=True, text=True)
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
    confirm = input(f"⚠️  Overwrite ALL data on {dev} with {img}? Type YES: ")
    if confirm != "YES":
        log("  Aborted.", lf)
        return
    log(f"→ Restoring {img} → {dev}...", lf)
    if WIN:
        log("  Windows: use Win32DiskImager to restore .img files.", lf)
        return
    subprocess.run(["dd", f"if={img}", f"of={dev}", "bs=4M", "status=progress", "conv=fsync"])
    # verify checksum if .sha256 exists
    sha_file = img + ".sha256"
    if os.path.isfile(sha_file):
        expected = open(sha_file).read().strip()
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
    TYPE_MAP = {
        "photos":    {"jpg","jpeg","png","gif","bmp","tiff","raw","cr2","nef","heic"},
        "videos":    {"mp4","mov","avi","mkv","3gp","wmv"},
        "audio":     {"mp3","wav","aac","flac","ogg","m4a"},
        "documents": {"pdf","doc","docx","xls","xlsx","ppt","txt"},
        "archives":  {"zip","rar","7z","tar","gz"},
    }
    moved = 0
    for root, _, files in os.walk(folder):
        # skip already-organized subdirs
        if any(os.path.basename(root) == cat for cat in TYPE_MAP):
            continue
        for fname in files:
            ext = fname.rsplit(".", 1)[-1].lower() if "." in fname else ""
            dest_cat = next((cat for cat, exts in TYPE_MAP.items() if ext in exts), "other")
            dest_dir = os.path.join(folder, dest_cat)
            os.makedirs(dest_dir, exist_ok=True)
            src = os.path.join(root, fname)
            dst = os.path.join(dest_dir, fname)
            # avoid collision
            if os.path.exists(dst):
                base, ext2 = os.path.splitext(fname)
                dst = os.path.join(dest_dir, f"{base}_{int(time.time())}{ext2}")
            shutil.move(src, dst)
            moved += 1
    log(f"  Organized {moved} file(s) into subfolders.", lf)

def dedup(folder, lf):
    log("→ Removing duplicate files...", lf)
    seen, removed = {}, 0
    for root, _, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(root, fname)
            try:
                digest = hashlib.md5()
                with open(fpath, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                        digest.update(chunk)
                h = digest.hexdigest()
                if h in seen:
                    os.remove(fpath)
                    removed += 1
                else:
                    seen[h] = fpath
            except:
                pass
    log(f"  Removed {removed} duplicate(s).", lf)

FILE_TYPES = {
    "1": ("Photos",    ["jpg", "png", "gif", "bmp", "tiff", "raw", "cr2", "nef"]),
    "2": ("Videos",    ["mp4", "mov", "avi", "mkv", "3gp", "wmv"]),
    "3": ("Audio",     ["mp3", "wav", "aac", "flac", "ogg", "m4a"]),
    "4": ("Documents", ["pdf", "doc", "docx", "xls", "xlsx", "txt"]),
    "5": ("Archives",  ["zip", "rar", "7z", "tar", "gz"]),
    "6": ("All",       []),
}
EXT_FAMILY = {
    "jpg":"jpg","jpeg":"jpg","png":"png","gif":"gif","bmp":"bmp","tiff":"tiff",
    "raw":"raw","cr2":"raw","nef":"raw","mp4":"mp4","mov":"mov","avi":"avi",
    "mkv":"mkv","3gp":"3gp","wmv":"wmv","mp3":"mp3","wav":"wav","aac":"aac",
    "flac":"flac","ogg":"ogg","m4a":"mp4","pdf":"pdf","doc":"doc","docx":"doc",
    "xls":"xls","xlsx":"xls","txt":"txt","zip":"zip","rar":"rar","7z":"7z",
    "tar":"tar","gz":"gz",
}

def pick_file_types(lf):
    print("\nFile type filter:")
    for k, (label, exts) in FILE_TYPES.items():
        print(f"  [{k}] {label:<12} ({', '.join(exts) if exts else 'everything'})")
    choices = input("Choose types (e.g. 1 2, default 6=All): ").strip() or "6"
    exts = []
    for c in choices.split():
        if c in FILE_TYPES:
            exts.extend(FILE_TYPES[c][1])
    if not exts:
        log("  File filter: All types", lf)
        return None
    log(f"  File filter: {', '.join(exts)}", lf)
    return exts

def write_photorec_cfg(exts, out_dir):
    families = sorted(set(EXT_FAMILY[e] for e in exts if e in EXT_FAMILY))
    cfg = os.path.join(out_dir, "photorec.cfg")
    with open(cfg, "w") as f:
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
    max_size  = input("  Max size in GB (e.g. 2.5): ").strip()

    log(f"→ Launching PhotoRec on {dev} → {out}", lf)
    print("  (Follow prompts — select partition then choose the output folder above)\n")
    subprocess.run([tool, dev])

    if max_files or max_size:
        all_files = sorted([
            os.path.join(r, f) for r, _, files in os.walk(out) for f in files
        ], key=os.path.getmtime)
        limit_count = int(max_files) if max_files else None
        limit_bytes = int(float(max_size) * 1024**3) if max_size else None
        kept = total = 0
        for fpath in all_files:
            sz = os.path.getsize(fpath)
            if (limit_count and kept >= limit_count) or (limit_bytes and total + sz > limit_bytes):
                os.remove(fpath)
            else:
                kept += 1; total += sz
        log(f"  Kept {kept} file(s) ({total/1024**2:.1f} MB)", lf)

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

    dev = sys.argv[1] if len(sys.argv) > 1 else input(
        "\nEnter device (e.g. /dev/sdb): ").strip()
    log(f"Device: {dev}", lf)

    print("\nMode:")
    print("  [1] format  - wipe/reformat")
    print("  [2] recover - recover files (PhotoRec)")
    print("  [3] health  - bad block + S.M.A.R.T. check")
    print("  [4] backup  - image the card to a .img file")
    print("  [5] speed   - benchmark read/write speed")
    print("  [6] info    - show card info")
    print("  [7] repair  - fix corrupted filesystem (no wipe)")
    print("  [8] restore - flash a .img backup back to card")
    mode = input("Choose [1-8] (default 1): ").strip() or "1"
    log(f"Mode: {mode}", lf)

    if mode == "2":
        do_recover(dev, lf)
        eject(dev, lf)
        sys.exit(0)
    if mode == "3":
        health_check(dev, lf)
        sys.exit(0)
    if mode == "4":
        backup(dev, lf)
        sys.exit(0)
    if mode == "5":
        speed_test(dev, lf)
        sys.exit(0)
    if mode == "6":
        card_info(dev, lf)
        sys.exit(0)
    if mode == "7":
        repair(dev, lf)
        sys.exit(0)
    if mode == "8":
        restore(dev, lf)
        eject(dev, lf)
        sys.exit(0)

    # Mode 1: format
    if input("\nRun health check before format? [y/N]: ").strip().lower() == "y":
        health_check(dev, lf)
    if input("Backup card before format? [y/N]: ").strip().lower() == "y":
        backup(dev, lf)

    confirm = input(f"\n⚠️  DESTROY all data on {dev}? Type YES: ")
    if confirm != "YES":
        log("Aborted.", lf)
        sys.exit(0)

    print("\nFormat level:")
    print("  [1] low      - reformat only")
    print("  [2] medium   - zero fill + reformat")
    print("  [3] high     - 3x random + zero fill + reformat")
    print("  [4] override - clear RO + high + reformat")
    level = input("Choose [1-4] (default 2): ").strip() or "2"
    fs    = input("Filesystem [fat32/exfat/ext4] (default fat32): ").strip() or "fat32"
    label = input("Volume label (optional, e.g. GOPRO): ").strip()
    log(f"Level: {level}, FS: {fs}, Label: {label or '(none)'}", lf)

    if level == "4":
        clear_ro(dev, lf)
    if level in ("2", "3", "4"):
        if level in ("3", "4"):
            for i in range(1, 4):
                dd_fill(dev, "random", f"Random pass {i}/3", lf)
        dd_fill(dev, "zero", "Zero fill", lf)

    do_format(dev, fs, label, lf)
    verify_write(dev, lf)
    speed_test(dev, lf)

    log(f"✓ Done. {dev} formatted as {fs}.", lf)
    print(f"\nLog saved to: {lf}")
    notify(f"cardnuke done: {dev} formatted as {fs}")
    eject(dev, lf)

if __name__ == "__main__":
    main()
