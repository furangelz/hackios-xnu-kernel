#!/usr/bin/env python3
"""
Show the UEFI's display snapshots side by side (lemonadep; XnuDisplaySnapshot.h in Mu-Silicium):
slot 0 = SEC before the SMMU detach (the bootloader's state), 1 = right after it, 2 = BDS after
all drivers. Fetch them with

  fastboot oem dispsnap && fastboot get_staged logs/dispsnap.bin
  tools/dispsnap.py logs/dispsnap.bin [--all]

Only registers that differ between slots are printed unless --all; SMMU stream groups are
decoded (stream id/mask, S2CR type and context bank) and those of the display marked.
"""
import struct, sys

SLOT_SIZE = 0x4000
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
for n in (12, 24, 82, 83):
    NAMES[0xF100000 + 0x1000 * n] = 'tlmm gpio%d cfg' % n
    NAMES[0xF100000 + 0x1000 * n + 4] = 'tlmm gpio%d in_out' % n


def slots(data):
    out = []
    for i in range(len(data) // SLOT_SIZE):
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
            print('%09x %-28s %s' % (a, NAMES.get(a, 'smmu cbar%d' % ((a - SMMU) % 0x10000 // 4) if a >> 24 == 0x15 else ''),
                                      '  '.join(vals)))

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


main()
