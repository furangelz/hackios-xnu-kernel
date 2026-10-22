#!/usr/bin/env python3
"""
Show the UEFI's display snapshots side by side (lemonadep; XnuDisplaySnapshot.h in Mu-Silicium):
slot 0 = SEC before the SMMU detach (the bootloader's state), 1 = right after it, 2 = BDS after
all drivers, 3 / 4 = right before / after ClockDxe; then raw dumps 0 / 1 (before / after
ClockDxe) of the display clock controller (36 KB) and GCC's display page. Fetch them with

  fastboot oem dispsnap && fastboot get_staged logs/dispsnap.bin
  tools/dispsnap.py logs/dispsnap.bin [--all]

Only registers that differ between slots are printed unless --all; SMMU stream groups are
decoded (stream id/mask, S2CR type and context bank) and those of the display marked.
"""
import struct, sys

SLOT_SIZE = 0x2000
SLOTS = 5
DUMP_DISPCC, DUMP_SIZE = 0x9000, 0xA000
MAGIC = 0x50414E53
SMMU = 0x15000000
DISPLAY_STREAMS = (0x820, 0x821, 0xC21)   # what Lahaina's PlatformSecLib keeps attached

NAMES = {
    0x127004: 'gcc disp_ahb_cbcr', 0x12700C: 'gcc disp_hf_axi_cbcr', 0x127014: 'gcc disp_sf_axi_cbcr',
    0x12701C: 'gcc disp_xo_cbcr',
    0xAF03000: 'dispcc mdss_gdscr', 0xAF02080: 'dispcc mdss_ahb_cbcr', 0xAF0200C: 'dispcc mdss_mdp_cbcr',
    0xAF02004: 'dispcc mdss_pclk0_cbcr', 0xAF02028: 'dispcc mdss_byte0_cbcr', 0xAF0202C: 'dispcc mdss_byte0_intf_cbcr',
    0xAF02038: 'dispcc mdss_esc0_cbcr', 0xAF02024: 'dispcc mdss_vsync_cbcr', 0xAF0201C: 'dispcc mdss_mdp_lut_cbcr',
    0xAF02014: 'dispcc mdss_rot_cbcr', 0xAF04004: 'dispcc mdss_non_gdsc_ahb_cbcr', 0xAF0605C: 'dispcc xo_cbcr',
    SMMU + 0x20: 'smmu id0', SMMU + 0x24: 'smmu id1',
}
SMMU_CB = SMMU + 128 * 0x1000          # ID1 0x6000004b: 4 KB pages, 128 global pages, 75 banks
CB_REGS = {0x00: 'sctlr', 0x20: 'ttbr0 lo', 0x24: 'ttbr0 hi', 0x30: 'tcr', 0x38: 'mair0', 0x58: 'fsr',
           0x60: 'far lo', 0x64: 'far hi', 0x68: 'fsynr0'}
for cb in range(75):
    for off, name in CB_REGS.items():
        NAMES[SMMU_CB + cb * 0x1000 + off] = 'smmu cb%d %s' % (cb, name)
for cb in range(128):
    NAMES.setdefault(SMMU + 0x1000 + 4 * cb, 'smmu cbar%d' % cb)
PLL_REGS = {0x00: 'mode', 0x04: 'l', 0x08: 'cal_l', 0x0C: 'user_ctl', 0x10: 'user_ctl_u', 0x14: 'user_ctl_u1',
            0x18: 'config_ctl', 0x1C: 'config_ctl_u', 0x20: 'config_ctl_u1', 0x30: 'status', 0x38: 'opmode', 0x40: 'alpha'}
for pll in (0, 1):
    for off, name in PLL_REGS.items():
        NAMES[0xAF00000 + pll * 0x1000 + off] = 'dispcc pll%d %s' % (pll, name)
for base, name in {0x22A0: 'ahb', 0x2110: 'byte0', 0x2148: 'esc0', 0x20C8: 'mdp', 0x2098: 'pclk0', 0x20E0: 'rot',
                   0x20F8: 'vsync'}.items():
    for i, reg in enumerate(('cmd', 'cfg', 'm', 'n', 'd')):
        NAMES[0xAF00000 + base + 4 * i] = 'dispcc %s_src %s' % (name, reg)
for n in (12, 24, 82, 83):
    NAMES[0xF100000 + 0x1000 * n] = 'tlmm gpio%d cfg' % n
    NAMES[0xF100000 + 0x1000 * n + 4] = 'tlmm gpio%d in_out' % n


DISPCC_RCG = {0x22a0: 'mdss_ahb_clk_src', 0x2110: 'mdss_byte0_clk_src', 0x212c: 'mdss_byte1_clk_src',
              0x2148: 'mdss_esc0_clk_src', 0x20c8: 'mdss_mdp_clk_src', 0x2098: 'mdss_pclk0_clk_src',
              0x20b0: 'mdss_pclk1_clk_src', 0x20e0: 'mdss_rot_clk_src', 0x20f8: 'mdss_vsync_clk_src'}


def dispcc_name(off):
    if off < 0x2000:
        return 'disp_cc_pll%d +%#x' % (off // 0x1000, off % 0x1000)
    for base, name in DISPCC_RCG.items():
        if base <= off < base + 0x18:
            return '%s %s' % (name, ('cmd', 'cfg', 'm', 'n', 'd', '?')[(off - base) // 4])
    a = 0xAF00000 + off
    return NAMES.get(a, '')


def dumps(data):
    out = []
    for i in range(2):
        b = SLOTS * SLOT_SIZE + i * DUMP_SIZE
        out.append(data[b:b + DUMP_SIZE] if len(data) >= b + DUMP_SIZE else None)
    return out


def slots(data):
    out = []
    for i in range(SLOTS):
        s = data[i * SLOT_SIZE:(i + 1) * SLOT_SIZE]
        magic, slot, count, skipped = struct.unpack_from('<4I', s)
        if magic != MAGIC or count > (SLOT_SIZE - 16) // 8:
            out.append(None)
            continue
        pairs = dict(struct.unpack_from('<%dI' % (2 * count), s, 16)[k:k + 2] for k in range(0, 2 * count, 2))
        out.append({'slot': slot, 'skipped': skipped, 'regs': pairs})
    return out


def smr(v):
    return 'valid' if v >> 31 else 'off', v & 0xFFFF, (v >> 16) & 0x7FFF


def s2cr(v):
    return ('translate', 'bypass', 'fault', '?')[(v >> 16) & 3], v & 0xFF


def main():
    data = open(sys.argv[1], 'rb').read()
    show_all = '--all' in sys.argv
    ss = slots(data)
    for i, s in enumerate(ss):
        if s is None:
            print('slot %d: none (magic missing: that stage did not run, or the RAM was not kept)' % i)
        else:
            print('slot %d: %d registers, skipped %#x' % (i, len(s['regs']), s['skipped']))
    present = [s for s in ss if s]
    if not present:
        return
    addrs = sorted(set().union(*(s['regs'] for s in present)))
    cell = lambda s, a: ('%08x' % s['regs'][a]) if s and a in s['regs'] else '-' * 8

    print('\n== registers%s ==' % ('' if show_all else ' that differ'))
    for a in addrs:
        if SMMU + 0x800 <= a < SMMU + 0x1000:
            continue
        vals = [cell(s, a) for s in ss]
        if show_all or len(set(vals)) > 1:
            print('%09x %-28s %s' % (a, NAMES.get(a, ''), '  '.join(vals)))

    print('\n== SMMU stream groups (SMR valid/id/mask, S2CR type/cb) ==')
    for g in range(128):
        sa, ca = SMMU + 0x800 + 4 * g, SMMU + 0xC00 + 4 * g
        row = [(s['regs'].get(sa), s['regs'].get(ca)) if s else (None, None) for s in ss]
        if all(r[0] is None or not r[0] >> 31 for r in row):
            continue
        txt = []
        mark = ''
        for sv, cv in row:
            if sv is None:
                txt.append('-')
                continue
            ok, sid, mask = smr(sv)
            typ, cb = s2cr(cv)
            txt.append('%s %04x/%04x %s cb%d' % (ok, sid, mask, typ, cb))
            if any((d & ~mask) == (sid & ~mask) for d in DISPLAY_STREAMS):
                mark = ' <- display'
        changed = len(set(map(str, row))) > 1
        if show_all or changed or mark:
            print('group %3d: %s%s' % (g, ' | '.join(txt), mark))


def main_dumps():
    data = open(sys.argv[1], 'rb').read()
    d = dumps(data)
    if not d[0] or not d[1]:
        return
    print('\n== around ClockDxe: raw dumps that differ (before -> after) ==')
    for off in range(0, DUMP_SIZE, 4):
        a, b = struct.unpack_from('<I', d[0], off)[0], struct.unpack_from('<I', d[1], off)[0]
        if a != b:
            if off < DUMP_DISPCC:
                print('dispcc +%#06x %-34s %08x -> %08x' % (off, dispcc_name(off), a, b))
            else:
                g = 0x127000 + off - DUMP_DISPCC
                print('gcc %#09x %-36s %08x -> %08x' % (g, NAMES.get(g, ''), a, b))


main()
main_dumps()
