#!/usr/bin/env python3
"""
build.py -- build a device's ramp patch from source and prove the result.

Compiled firmware is never published in this repo. This builds the injected
code blob locally with the real TC32 toolchain, works out every call-site
patch against YOUR stock firmware file, verifies all of it, and writes the
verified patch table into that device's apply_patch.py.

    python3 tools/build.py carta2 --firmware your-stock-firmware.bin
    python3 tools/build.py aeris  --firmware ...
    python3 tools/build.py sport  --firmware ...

Needs Docker and a Linux TC32 toolchain directory (tc32-elf-gcc/as/ld/objcopy/
objdump/nm in <dir>/bin), given by --toolchain or $TC32_TOOLCHAIN.

What it checks (a single FAIL stops it before apply_patch.py is touched):
  - no undefined symbols; the blob fits before the waypoint sector
  - every call from the blob into stock code targets an ODD address -- TC32
    `tjex` treats bit 0 like Arm `bx`, so an even target would fault
  - each call site's original bytes decode to exactly the expected stock
    instruction; every caller of a hooked stock function is found and the count
    matches what the patch expects, so none can be silently missed
  - each replacement `tjl` (from the real assembler) decodes to land exactly on
    its wrapper
  - end to end: apply_patch.py on your file -> every site holds its
    replacement, zero other body bytes change, the gap is 0xFF, the blob sits
    exactly at its address, the waypoint sector is erased, the header length and
    Telink CRC32 trailer are correct
"""
import argparse, hashlib, os, re, shutil, struct, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = 40
BASE = 0x28   # code runs at disassembly address + 0x28 (the header sits at flash 0)

# sites: (address, wrapper, expected stock instruction)
# hook_all: (stock target, wrapper, expected number of callers) -- every caller is patched
DEVICES = {
    'carta2': dict(
        src=['common/ramp_core.c', 'common/ramp_store.c', 'carta2/ramp_display.c', 'carta2/ramp_input.c'],
        asm=['carta2/ramp_marker_entry.s'],
        inject=0x30000, wp=0x32000, end=0x33000,
        sites=[(0x6e2e, 'ramp_trampoline', 'tjl 0xaf2c'),
               (0x11d96, 'ramp_marker_entry', 'tmovs r3, #40'),
               (0x6d0c, 'ramp_event_entry', 'tjl 0x5618'),
               (0xfa38, 'ramp_d3c0_full', 'tjl 0xd3c0'),
               (0xf546, 'ramp_d3c0_view', 'tjl 0xd3c0'),
               (0xfa3c, 'ramp_dcac_full', 'tjl 0xdcac'),
               (0xf39c, 'ramp_dcac_view', 'tjl 0xdcac')],
        hook_all=[('0xce70', 'ramp_ce70_hide', 7),
                  ('0xcf58', 'ramp_cf58_hide', 7),
                  ('0xdb40', 'ramp_db40_hide', 3),
                  # the stock bottom row, y 193-225: dab counter, mode icon,
                  # status icon, READY banner -- hidden so the ramp screen can
                  # use it
                  ('0xd048', 'ramp_d048_hide', 2),
                  ('0xe42c', 'ramp_e42c_hide', 2),
                  ('0xe300', 'ramp_e300_hide', 2),
                  ('0xe2b4', 'ramp_e2b4_hide', 2)],
        own_callers={'0xd3c0': 2, '0xdcac': 2, '0x5618': 1, '0xaf2c': 1}),
    'aeris': dict(
        src=['common/ramp_core.c', 'common/ramp_store.c', 'aeris/ramp_led.c'],
        asm=['aeris/ramp_marker_entry.s'],
        inject=0x14000, wp=0x15000, end=0x20000,
        sites=[(0x6464, 'ramp_trampoline', 'tjl 0x8154'),
               (0xb490, 'ramp_marker_entry', 'tmovs r3, #53')],
        hook_all=[],
        own_callers={'0x8154': 1}),
    'sport': dict(
        src=['common/ramp_core.c', 'common/ramp_store.c', 'sport/ramp_led.c'],
        asm=['sport/ramp_marker_entry.s'],
        inject=0x18000, wp=0x19000, end=0x20000,
        sites=[(0x58b0, 'ramp_trampoline', 'tjl 0x7c00'),
               (0xb002, 'ramp_marker_entry', 'tmovs r3, #53')],
        hook_all=[],
        own_callers={'0x7c00': 1}),
}

FAILS = []
def check(ok, msg):
    print(('  PASS ' if ok else '  FAIL ') + msg)
    if not ok:
        FAILS.append(msg)


def run_tc32(toolchain, work, cmd):
    """Run a shell command in ubuntu:22.04 with the repo, toolchain and work dir mounted."""
    r = subprocess.run(['docker', 'run', '--rm',
                        '-v', f'{REPO}:/repo', '-v', f'{toolchain}:/tc', '-v', f'{work}:/work',
                        'ubuntu:22.04', 'sh', '-c', 'export PATH=/tc/bin:$PATH; set -e; ' + cmd],
                       capture_output=True, text=True,
                       env={**os.environ, 'MSYS_NO_PATHCONV': '1'})
    if r.returncode:
        sys.exit(f'toolchain step failed:\n{r.stdout}\n{r.stderr}')
    return r.stdout


def tjl_lands(site, enc):
    hw1, hw2 = int.from_bytes(enc[0:2], 'little'), int.from_bytes(enc[2:4], 'little')
    off = ((hw1 & 0x7ff) << 12) | ((hw2 & 0x7ff) << 1)
    if off & (1 << 22):
        off -= 1 << 23
    return site + 4 + off


def crc32_telink(data):
    c = 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    return c


def parse_dis(text):
    ins, order, words = {}, [], {}
    for l in text.splitlines():
        f = l.split('\t')
        if len(f) >= 4 and f[0].strip().endswith(':'):
            try:
                a = int(f[0].strip()[:-1], 16)
            except ValueError:
                continue
            ins[a] = (f[2].strip(), f[3].split(';')[0].strip(), '	'.join(f[3:])); order.append(a)
        m = re.match(r'^\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+\.word\s+0x([0-9a-f]+)', l)
        if m:
            words[int(m.group(1), 16)] = int(m.group(3), 16)
    return ins, order, words


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('device', choices=DEVICES)
    ap.add_argument('--firmware', required=True, help='your own stock firmware file, as downloaded')
    ap.add_argument('--toolchain', default=os.environ.get('TC32_TOOLCHAIN'))
    ap.add_argument('--keep', metavar='DIR', help='copy the built blob, symbols and disassembly here (for tools/simulate.py)')
    a = ap.parse_args()
    if not a.toolchain:
        sys.exit('give --toolchain or set TC32_TOOLCHAIN')
    D = DEVICES[a.device]
    work = tempfile.mkdtemp(prefix='ramp-build-', dir=REPO)
    try:
        stock = open(a.firmware, 'rb').read()
        open(f'{work}/stripped.bin', 'wb').write(stock[HDR:])

        print(f'== {a.device}: build ==')
        dev = a.device
        cmd = ''.join(f'tc32-elf-gcc -mtc32 -ffreestanding -nostdlib -Os -Wall -Werror '
                      f'-I/repo/{dev} -I/repo/common -c /repo/{s} -o /work/{os.path.basename(s)}.o; '
                      for s in D['src'])
        cmd += ''.join(f'tc32-elf-as /repo/{s} -o /work/{os.path.basename(s)}.o; ' for s in D['asm'])
        objs = ' '.join(f'/work/{os.path.basename(s)}.o' for s in D['src'] + D['asm'])
        cmd += (f'tc32-elf-ld -Ttext={D["inject"] + BASE:#x} -o /work/r.elf {objs} 2>/dev/null; '
                'tc32-elf-objcopy -O binary /work/r.elf /work/r.bin; '
                'tc32-elf-nm /work/r.elf > /work/nm; tc32-elf-nm -u /work/r.elf > /work/undef; '
                'tc32-elf-objdump -d /work/r.elf > /work/blob.dis; '
                'tc32-elf-objdump -D -b binary -m tc32 /work/stripped.bin > /work/stock.dis')
        run_tc32(a.toolchain, work, cmd)
        sym = {p[2]: int(p[0], 16) for p in (l.split() for l in open(f'{work}/nm')) if len(p) == 3 and p[1] == 'T'}
        undef = [l.split()[-1] for l in open(f'{work}/undef') if l.strip()]
        blob = open(f'{work}/r.bin', 'rb').read()

        sins, sorder, _ = parse_dis(open(f'{work}/stock.dis').read())
        entries = {x for x in sorder if sins[x][0] == 'tpush' and 'lr' in sins[x][1]}
        entries |= {int(sins[x][1].split()[0], 16) for x in sorder
                    if sins[x][0] == 'tjl' and re.match(r'0x[0-9a-f]+$', sins[x][1])}   # leaf stubs called directly

        print(f'== {a.device}: blob ==')
        check(undef == ['_start'], f'no undefined symbols besides _start ({undef})')
        check(D['inject'] + BASE + len(blob) <= D['wp'], f'{len(blob)} B running at {D["inject"] + BASE:#x} ends before the waypoint sector {D["wp"]:#x}')
        ins, order, words = parse_dis(open(f'{work}/blob.dis').read())
        n, even = 0, []
        for i, x in enumerate(order):
            op, args, _ = ins[x]
            m = re.match(r'([0-9a-f]+)', args) if op == 'tjl' else None
            if not m or int(m.group(1), 16) not in ins or ins[int(m.group(1), 16)][0] != 'tjex':
                continue
            reg = ins[int(m.group(1), 16)][1].split()[0]
            for k in range(i - 1, max(i - 10, 0), -1):
                op2, _, raw = ins[order[k]]
                mm = re.search(rf'tloadr\s*$', op2) and re.match(rf'{reg}, \[pc, #\d+\]', ins[order[k]][1])
                if mm:
                    lit = int(re.search(r'\(([0-9a-f]+)', raw).group(1), 16)
                    v = words.get(lit); n += 1
                    if v is None or not v & 1 or (v - 1 - BASE) not in entries:
                        even.append(f'{x:#x}->{v:#x}' if v is not None else hex(x))
                    break
        check(n > 0 and not even, f'{n} calls into stock code: every target odd and exactly 0x28 past a real function entry (bad: {even or "none"})')

        print(f'== {a.device}: call sites ==')
        def at(addr):
            return ' '.join(sins[addr][:2]) if addr in sins else '?'
        for tgt, cnt in D['own_callers'].items():
            got = [x for x in sorder if sins[x][0] == 'tjl' and sins[x][1] == tgt]
            check(len(got) == cnt, f'{tgt} has exactly {cnt} caller(s) in this image (found {len(got)})')
        sites = list(D['sites'])
        for tgt, wrapper, cnt in D['hook_all']:
            got = [x for x in sorder if sins[x][0] == 'tjl' and sins[x][1] == tgt]
            check(len(got) == cnt, f'every caller of {tgt} hooked: {cnt} expected, {len(got)} found')
            sites += [(x, wrapper, f'tjl {tgt}') for x in got]
        cs = f'{work}/cs'; os.makedirs(cs)
        for i, (x, w, _) in enumerate(sites):
            open(f'{cs}/s{i}.s', 'w').write(f'.org {x + BASE:#x}\ntjl {sym[w]:#x}\n')
        run_tc32(a.toolchain, work, 'cd /work/cs; for f in s*.s; do b=${f%.s}; tc32-elf-as $f -o $b.o; tc32-elf-objcopy -O binary $b.o $b.bin; done')
        table = []
        for i, (x, w, expect) in enumerate(sites):
            new = open(f'{cs}/s{i}.bin', 'rb').read()[-4:]
            old = stock[HDR + x:HDR + x + 4]
            ok = at(x) == expect and tjl_lands(x + BASE, new) == sym[w]
            check(ok, f'{x:#07x} [{at(x)}] {old.hex()} -> {new.hex()} -> {w} @ {tjl_lands(x + BASE, new):#x}')
            table.append((x, w, expect, old.hex(), new.hex()))
        if FAILS:
            sys.exit(f'\n{len(FAILS)} check(s) failed -- nothing written')

        print(f'== {a.device}: apply_patch.py ==')
        app = f'{REPO}/{dev}/apply_patch.py'
        text = open(app).read()
        body = ''.join(f'    (0x{x:X}, bytes.fromhex("{o}"), bytes.fromhex("{nw}")),  # {e} -> {w}\n'
                       for x, w, e, o, nw in sorted(table))
        text, k = re.subn(r'PATCHES = \[\n.*?\n\]\n', lambda _: 'PATCHES = [\n' + body + ']\n', text, flags=re.S)
        check(k == 1, f'patch table written ({len(table)} sites)')
        text, k = re.subn(r'BLOB_SHA256 = "[0-9a-f]{64}"', f'BLOB_SHA256 = "{hashlib.sha256(blob).hexdigest()}"', text)
        check(k == 1, 'blob hash written')
        open(app, 'w', newline='\n').write(text)
        open(f'{REPO}/{dev}/ramp_firmware_v1.bin', 'wb').write(blob)   # local only; .gitignore'd

        print(f'== {a.device}: end to end ==')
        out_path = f'{work}/patched.bin'
        r = subprocess.run([sys.executable, app, '--input', a.firmware, '--output', out_path], capture_output=True, text=True)
        check(r.returncode == 0, f'apply_patch.py ran ({r.stderr.strip()[:150]})')
        out = open(out_path, 'rb').read()
        blen = len(stock) - HDR
        touched = {x + k for x, *_ in table for k in range(4)}
        check(all(out[HDR + x:HDR + x + 4] == bytes.fromhex(nw) for x, _, _, _, nw in table), 'every site holds its replacement')
        stray = sum(1 for i in range(blen) if i not in touched and out[HDR + i] != stock[HDR + i])
        check(stray == 0, f'zero bytes changed outside the patch sites ({stray})')
        check(set(out[HDR + blen:HDR + D['inject']]) == {0xFF}, 'gap up to the blob is 0xFF')
        check(out[HDR + D['inject']:HDR + D['inject'] + len(blob)] == blob, 'blob placed exactly at its address')
        check(len(out) >= D['wp'] + 0x1000 and set(out[D['wp']:D['wp'] + 0x1000]) == {0xFF}, f'waypoint sector (flash {D["wp"]:#x}) erased in the image')
        check(struct.unpack('<I', out[24:28])[0] == len(out), 'header length field == file size')
        check(struct.unpack('<I', out[-4:])[0] == crc32_telink(out[:-4]), 'Telink CRC32 trailer correct')
        check(out[8:12] == b'KNLT' and out[:24] == stock[:24], 'header intact')
        check(len(out) == D['end'] + 4, f'image covers exactly flash 0..{D["end"]:#x} (+ trailer): no sector past it is touched')

        print(f'== {a.device}: independent decode ==')
        # The checks above decode each site with tjl_lands(); this re-reads the
        # FINISHED image with the real disassembler instead, as a cross-check.
        open(f'{work}/final_body.bin', 'wb').write(out[HDR:-4])
        fdis = run_tc32(a.toolchain, work, 'tc32-elf-objdump -D -b binary -m tc32 /work/final_body.bin')
        fins = {}
        for l in fdis.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\t[0-9a-f ]+\t(\S+)\t?(.*)', l)
            if m:
                fins[int(m.group(1), 16)] = (m.group(2), m.group(3).split(';')[0].strip())
        bad = []
        for x, w, _, _, _ in table:
            op, arg = fins.get(x, ('?', '?'))
            if op != 'tjl' or not arg or int(arg.split()[0], 16) != sym[w] - BASE:
                bad.append(f'{x:#x}: {op} {arg}')
        check(not bad, f'objdump of the finished image: all {len(table)} sites are tjl to their function (bad: {bad or "none"})')

        print(f'== {a.device}: refusals ==')
        def refuses(fw_bytes, label, blob_override=None):
            fw, o = f'{work}/neg_in.bin', f'{work}/neg_out.bin'
            open(fw, 'wb').write(fw_bytes)
            blob_path = f'{REPO}/{dev}/ramp_firmware_v1.bin'
            if blob_override is not None:
                open(blob_path, 'wb').write(blob_override)
            try:
                if os.path.exists(o):
                    os.remove(o)
                r = subprocess.run([sys.executable, app, '--input', fw, '--output', o], capture_output=True, text=True)
            finally:
                open(blob_path, 'wb').write(blob)
            check(r.returncode != 0 and not os.path.exists(o), f'refuses {label}, writes nothing')
        site = table[0][0]
        tampered = bytearray(stock); tampered[HDR + site] ^= 0xFF
        refuses(bytes(tampered), 'a firmware with a patch site altered')
        tampered = bytearray(stock); tampered[HDR + 0x100] ^= 0x01
        refuses(bytes(tampered), 'a different build (one bit off, sites intact)')
        refuses(stock[:HDR + 1000], 'a truncated file')
        refuses(stock, 'a code blob other than the one the table was built for', blob_override=blob[:100] + bytes([blob[100] ^ 1]) + blob[101:])
        if a.keep:
            os.makedirs(a.keep, exist_ok=True)
            for f in ('r.bin', 'r.elf', 'nm', 'blob.dis', 'stripped.bin'):
                shutil.copy(f'{work}/{f}', a.keep)
        print(f'\n{a.device}: ' + ('ALL CHECKS PASSED' if not FAILS else f'{len(FAILS)} FAILED'))
        sys.exit(1 if FAILS else 0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
