#!/usr/bin/env bash
# Safety and behaviour tests for cardnuke. They use disk image files (--allow-file), so no real card is touched.
# Run from the repository root:  make test
set -u
BIN=${BIN:-./cardnuke}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()   { echo "  ok    $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

make_card() {
  python3 - "$1" <<'PY'
import sys
d = bytearray(64*1024*1024)
d[4096:4096+24] = b"MY WEDDING PHOTOS (JPEG)"
d[40*1024*1024:40*1024*1024+8] = b"KEEP-ME!"
open(sys.argv[1], "wb").write(d)
PY
}
sum() { md5sum "$1" | cut -d' ' -f1; }

echo "cardnuke tests"
make_card "$TMP/card.img"; before=$(sum "$TMP/card.img")

echo "- refusing bad input"
$BIN "$TMP/card.img" speed >/dev/null 2>&1;                       check "refuses a plain file without --allow-file" "[ \$? -ne 0 ]"
$BIN '/dev/nope; touch '"$TMP"'/pwned' speed >/dev/null 2>&1;    check "a shell command in the device name is not run" "[ ! -e '$TMP/pwned' ]"
$BIN "$TMP/card.img" format ntfs X --allow-file --dry-run >/dev/null 2>&1; check "rejects an unknown filesystem" "[ \$? -ne 0 ]"
$BIN "$TMP/card.img" format exfat 'a"; touch '"$TMP"'/pwned2; "' --allow-file --dry-run >/dev/null 2>&1
check "a shell command in the label is not run" "[ ! -e '$TMP/pwned2' ]"

echo "- the checks leave data alone"
$BIN "$TMP/card.img" speed --allow-file >/dev/null 2>&1;          check "speed test does not change the card" "[ \"\$(sum '$TMP/card.img')\" = '$before' ]"
$BIN "$TMP/card.img" health --allow-file >/dev/null 2>&1 || true; check "health scan does not change the card" "[ \"\$(sum '$TMP/card.img')\" = '$before' ]"

echo "- destructive actions need a yes"
$BIN "$TMP/card.img" speed --destructive --allow-file </dev/null >/dev/null 2>&1
check "raw write test refuses without confirmation" "[ \"\$(sum '$TMP/card.img')\" = '$before' ]"
$BIN "$TMP/card.img" restore "$TMP/nothing.img" --allow-file --yes >/dev/null 2>&1; check "restore of a missing image fails cleanly" "[ \$? -ne 0 ]"

echo "- backup and restore"
$BIN "$TMP/card.img" backup "$TMP/b.img" --allow-file >/dev/null 2>&1
check "backup is identical to the card" "cmp -s '$TMP/card.img' '$TMP/b.img'"
check "backup writes a checksum" "[ -s '$TMP/b.img.sha256' ]"
$BIN "$TMP/card.img" backup "$TMP/b.img" --allow-file >/dev/null 2>&1; check "backup will not overwrite without --force" "[ \$? -ne 0 ]"
cp "$TMP/card.img" "$TMP/target.img"
$BIN "$TMP/target.img" restore "$TMP/b.img" --allow-file --yes >/dev/null 2>&1; check "restore of a good image works" "cmp -s '$TMP/target.img' '$TMP/b.img'"
python3 - "$TMP" <<'PY'
import sys; t=sys.argv[1]
d=bytearray(open(t+"/b.img","rb").read()); d[100]^=0xFF; open(t+"/bad.img","wb").write(d)
open(t+"/bad.img.sha256","w").write(open(t+"/b.img.sha256").read().replace("b.img","bad.img"))
PY
cp "$TMP/card.img" "$TMP/target2.img"
$BIN "$TMP/target2.img" restore "$TMP/bad.img" --allow-file --yes >/dev/null 2>&1
check "a damaged image is refused and nothing is written" "[ \$? -ne 0 ] && cmp -s '$TMP/target2.img' '$TMP/card.img'"
head -c 100000000 /dev/zero > "$TMP/big.img"
$BIN "$TMP/target2.img" restore "$TMP/big.img" --allow-file --yes >/dev/null 2>&1; check "an image bigger than the device is refused" "[ \$? -ne 0 ]"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
