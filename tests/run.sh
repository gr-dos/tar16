#!/bin/sh
#
# Test tar.exe under DOSBox-X.
#
# Fixtures are made on the host (foreign archives with the host's GNU
# tar), one batch file runs every DOS-side step, and the results are
# checked on the host afterwards.  Each step N records tar's exit
# status in RN.TXT and its standard output in ON.TXT.  DOS cannot
# redirect standard error, so diagnostics are not checked.  rc.bat
# uses goto rather than "if errorlevel N echo ... >file" because the
# DOSBox-X shell opens the redirection even when the test is false.
#
# Usage: tests/run.sh [path/to/tar.exe]
# Environment: DOSBOX (default dosbox-x), HOSTTAR (default tar),
#              KEEP=1 to keep the work directory.

set -u

top=$(cd "$(dirname "$0")/.." && pwd)
exe=${1:-$top/tar.exe}
DOSBOX=${DOSBOX:-dosbox-x}
HOSTTAR=${HOSTTAR:-tar}

command -v "$DOSBOX" >/dev/null || { echo "$DOSBOX not found" >&2; exit 2; }
"$HOSTTAR" --version 2>/dev/null | grep -q GNU ||
    { echo "$HOSTTAR is not GNU tar" >&2; exit 2; }
[ -f "$exe" ] || { echo "$exe not found" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/dostar.XXXXXX")
pass=0
fail=0

# Everything runs in UTC: DOSBox-X presents host times in host local
# time, and the batch file sets TZ=UTC0 for the runtime.
TZ=UTC
export TZ
STAMP='2001-02-03 04:05:06'
STAMPT=$(date -d "$STAMP" +%s)
LC_ALL=C
export LC_ALL

# ------------------------------------------------------------------
# fixtures

cd "$work" || exit 2
cp "$exe" tar.exe

mkdir -p src/sub
printf 'hello\n' > src/a.txt
: > src/empty.dat
head -c 3000 /dev/urandom > src/sub/b.bin
head -c 200000 /dev/urandom > src/big.bin
find src -type f -exec touch -d "$STAMP" {} +

mkdir -p wild/sub
echo one > wild/one.c
echo two > wild/two.c
echo hdr > wild/x.h
echo s > wild/sub/s.c

mkdir self
echo f > self/f.txt

mkdir -p fsrc/q/r
(
    cd fsrc || exit 2
    echo one > q/r/one.txt
    ln q/r/one.txt q/hard.txt
    ln -s r/one.txt q/sym.txt
    echo ro > q/ro.txt
    chmod 644 q/r/one.txt
    chmod 444 q/ro.txt
    touch -d "$STAMP" q/r/one.txt q/ro.txt
    deep=aaaaaaaa/bbbbbbbb/cccccccc/dddddddd/eeeeeeee/ffffffff/gggggggg
    deep=$deep/hhhhhhhh/iiiiiiii/jjjjjjjj/kkkkkkkk/llllllll
    mkdir -p $deep
    echo deep > $deep/f.txt
    long=q/$(printf '%0120d' 0 | tr 0 x).txt
    echo long > "$long"
    chmod 644 "$long"
    touch -d "$STAMP" "$long"
    opts='--owner=0 --group=0 --numeric-owner'
    $HOSTTAR $opts --format=ustar -cf ../ustar.tar \
        q/r/one.txt q/hard.txt q/sym.txt q/ro.txt
    $HOSTTAR $opts --format=ustar -cf ../deep.tar $deep/f.txt
    $HOSTTAR $opts --format=pax -cf ../pax.tar q/r/one.txt "$long"
    rm "$long"
    $HOSTTAR $opts --format=v7 -cf ../v7.tar q
)

cat > rc.bat <<'EOF'
@echo off
if errorlevel 2 goto rc2
if errorlevel 1 goto rc1
echo rc0 >c:\r%1.txt
goto end
:rc1
echo rc1 >c:\r%1.txt
goto end
:rc2
echo rc2 >c:\r%1.txt
:end
EOF

cat > steps.bat <<'EOF'
@echo off
rem --- create, list, extract
tar cvf t1.tar src >o1.txt
call rc 1
tar tvf t1.tar >o2.txt
call rc 2
mkdir x1
cd x1
..\tar xvf ..\t1.tar >..\o3.txt
call ..\rc 3
cd ..
rem --- archive on standard output
tar cf - src >t4.tar
call rc 4
rem --- blocking factor
tar cbf 1 t5.tar src\a.txt
call rc 5
rem --- read-only file
attrib +r src\a.txt
tar cf t6.tar src\a.txt
call rc 6
attrib -r src\a.txt
rem --- operand selects a directory
tar tf t1.tar src/sub >o7.txt
call rc 7
rem --- errors
tar tf t1.tar nothere
call rc 8
tar xf nothere.tar
call rc 9
tar
call rc 10
tar tf tar.exe
call rc 11
rem --- m: do not restore mtime
mkdir x12
cd x12
..\tar xmf ..\t1.tar src/a.txt
call ..\rc 12
cd ..
rem --- the archive is not added to itself
tar cf self\s.tar self
call rc 13
rem --- u and r
copy t1.tar t14.tar >nul
echo new >src\new.txt
tar uvf t14.tar src >o14.txt
call rc 14
tar rf t14.tar src\a.txt
call rc 15
tar tf t14.tar >o15.txt
rem --- foreign archives
mkdir x16
cd x16
..\tar xf ..\ustar.tar
call ..\rc 16
cd ..
tar tf deep.tar >o17.txt
call rc 17
tar tvf pax.tar >o18.txt
call rc 18
mkdir x19
cd x19
..\tar xf ..\v7.tar
call ..\rc 19
cd ..
rem --- wildcards
cd wild
..\tar cvf ..\w20.tar *.c >..\o20.txt
call ..\rc 20
cd ..
tar cf w21.tar wild\*.c wild\s*
call rc 21
tar tf w21.tar >o21.txt
tar cf w22.tar wild
tar tf w22.tar *.c >o22.txt
call rc 22
tar tf w22.tar wild/[ot]*.? >o23.txt
call rc 23
tar tf w22.tar WILD/SU? >o24.txt
call rc 24
tar tf w22.tar *.zz
call rc 25
tar cf w26.tar *.zz
call rc 26
mkdir x27
cd x27
..\tar xf ..\w22.tar *.c
call ..\rc 27
cd ..
rem --- verbose listing format
tar tvf t1.tar src/a.txt >o28.txt
call rc 28
rem --- version
tar --version >o29.txt
call rc 29
EOF

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 \
    "$DOSBOX" -defaultconf -silent -nogui -nomenu \
    -set "cpu cycles=max" \
    -c "mount c \"$work\"" -c "c:" -c "set TZ=UTC0" \
    -c "steps.bat" -c "exit" >dosbox.log 2>&1

# ------------------------------------------------------------------
# checks

ok()     { pass=$((pass + 1)); }
not_ok() { echo "FAIL: $*"; fail=$((fail + 1)); }

# check NAME COMMAND...
check() {
    name=$1
    shift
    if "$@"; then ok; else not_ok "$name"; fi
}

# rc STEP EXPECTED-STATUS
rc() {
    got=$(tr -d ' \r' < "R$1.TXT" 2>/dev/null | sed 's/^rc//')
    if [ "$got" = "$2" ]; then
        ok
    else
        not_ok "step $1: exit status ${got:-missing}, expected $2"
    fi
}

# expect STEP < expected-lines; compares sorted ON.TXT
expect() {
    tr -d '\r' < "O$1.TXT" 2>/dev/null | sort > got.txt
    sort > want.txt
    if cmp -s got.txt want.txt; then
        ok
    else
        not_ok "step $1: output differs"
        diff want.txt got.txt | sed 's/^/    /'
    fi
}

mtime() { stat -c %Y "$1"; }
size()  { stat -c %s "$1"; }

# create, list, extract
rc 1 0
expect 1 <<'EOF'
a src/
a src/a.txt, 6 bytes
a src/big.bin, 200000 bytes
a src/empty.dat, 0 bytes
a src/sub/
a src/sub/b.bin, 3000 bytes
EOF
rc 2 0
tr -d '\r' < O2.TXT | grep -v '^d' | sort > got.txt
cat > want.txt <<'EOF'
-rw-r--r-- 0/0        0 Feb 03 04:05 2001 src/empty.dat
-rw-r--r-- 0/0        6 Feb 03 04:05 2001 src/a.txt
-rw-r--r-- 0/0     3000 Feb 03 04:05 2001 src/sub/b.bin
-rw-r--r-- 0/0   200000 Feb 03 04:05 2001 src/big.bin
EOF
sort -o want.txt want.txt
check "step 2: verbose listing" cmp -s got.txt want.txt
rc 3 0
for f in a.txt empty.dat sub/b.bin big.bin; do
    F=X1/SRC/$(echo "$f" | tr a-z A-Z)
    check "step 3: $f contents" cmp -s "src/$f" "$F"
    check "step 3: $f mtime" [ "$(mtime "$F" 2>/dev/null)" = "$STAMPT" ]
done
check "step 3: GNU tar reads archive" "$HOSTTAR" tf T1.TAR >/dev/null
check "step 3: GNU tar data" \
    sh -c "$HOSTTAR xOf T1.TAR src/big.bin | cmp -s - src/big.bin"
check "step 3: ustar magic" \
    [ "$(dd if=T1.TAR bs=1 skip=257 count=8 2>/dev/null | od -An -c |
         tr -d ' ')" = 'ustar\000' ]
check "step 3: record size" [ $(($(size T1.TAR) % 10240)) -eq 0 ]

# standard output, blocking factor, read-only
rc 4 0
check "step 4: stdout archive matches file archive" cmp -s T1.TAR T4.TAR
rc 5 0
check "step 5: b 1 archive size" [ "$(size T5.TAR)" -eq 2048 ]
rc 6 0
check "step 6: read-only mode stored" \
    sh -c "$HOSTTAR tvf T6.TAR | grep -q '^-r--r--r-- '"

# operand selection and errors
rc 7 0
expect 7 <<'EOF'
src/sub/
src/sub/b.bin
EOF
rc 8 1
rc 9 2
rc 10 2
rc 11 2

rc 12 0
check "step 12: m leaves mtime unrestored" \
    [ "$(mtime X12/SRC/A.TXT 2>/dev/null)" != "$STAMPT" ]

rc 13 0
check "step 13: archive not added to itself" \
    sh -c "$HOSTTAR tf self/S.TAR | grep -qx self/f.txt &&
           ! $HOSTTAR tf self/S.TAR | grep -q s.tar"

# u and r
rc 14 0
check "step 14: u adds new file" grep -q 'a src/new.txt' O14.TXT
check "step 14: u skips unchanged files" \
    sh -c "! grep -Eq 'a src/(a.txt|big.bin|empty.dat|sub/b.bin)' O14.TXT"
rc 15 0
check "step 15: r appends" [ "$(grep -c '^src/a.txt' O15.TXT)" -eq 2 ]
check "step 15: one copy of new file" \
    [ "$(grep -c '^src/new.txt' O15.TXT)" -eq 1 ]
check "step 15: GNU tar reads updated archive" "$HOSTTAR" tf T14.TAR >/dev/null

# foreign archives
rc 16 1     # the symbolic link cannot be extracted
check "step 16: file" grep -qx one X16/Q/R/ONE.TXT
check "step 16: hard link copied" grep -qx one X16/Q/HARD.TXT
check "step 16: read-only restored" [ ! -w X16/Q/RO.TXT ]
check "step 16: mtime" [ "$(mtime X16/Q/R/ONE.TXT 2>/dev/null)" = "$STAMPT" ]
check "step 16: no symlink" [ ! -e X16/Q/SYM.TXT ]
rc 17 0
expect 17 <<'EOF'
aaaaaaaa/bbbbbbbb/cccccccc/dddddddd/eeeeeeee/ffffffff/gggggggg/hhhhhhhh/iiiiiiii/jjjjjjjj/kkkkkkkk/llllllll/f.txt
EOF
rc 18 0
expect 18 <<EOF
-rw-r--r-- 0/0        4 Feb 03 04:05 2001 q/r/one.txt
-rw-r--r-- 0/0        5 Feb 03 04:05 2001 q/$(printf '%0120d' 0 | tr 0 x).txt
EOF
rc 19 1     # the symbolic link cannot be extracted
check "step 19: v7 file" grep -qx one X19/Q/R/ONE.TXT
check "step 19: v7 directory" [ -d X19/Q/R ]

# wildcards
rc 20 0
expect 20 <<'EOF'
a one.c, 4 bytes
a two.c, 4 bytes
EOF
rc 21 0
expect 21 <<'EOF'
wild/one.c
wild/sub/
wild/sub/s.c
wild/two.c
EOF
rc 22 0
expect 22 <<'EOF'
wild/one.c
wild/sub/s.c
wild/two.c
EOF
rc 23 0
expect 23 <<'EOF'
wild/one.c
wild/two.c
EOF
rc 24 0
expect 24 <<'EOF'
wild/sub/
wild/sub/s.c
EOF
rc 25 1
rc 26 1
rc 27 0
check "step 27: wildcard extract" \
    [ -f X27/WILD/ONE.C -a -f X27/WILD/TWO.C -a -f X27/WILD/SUB/S.C ]
check "step 27: wildcard extract skips others" [ ! -e X27/WILD/X.H ]

rc 28 0
expect 28 <<'EOF'
-rw-r--r-- 0/0        6 Feb 03 04:05 2001 src/a.txt
EOF

rc 29 0
expect 29 <<EOF
tar (dos-tar16) $(cat "$top/VERSION")
EOF

# ------------------------------------------------------------------

echo "$pass passed, $fail failed"
if [ "$fail" -ne 0 ] || [ "${KEEP:-0}" = 1 ]; then
    echo "work directory: $work"
else
    rm -rf "$work"
fi
[ "$fail" -eq 0 ]
