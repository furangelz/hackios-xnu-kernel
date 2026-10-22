#!/usr/bin/env python3
"""
lemonadep's ClockDxe: the OnePlus 9's ClockDxe.patched.efi (Binaries/lemonade, Gus Monce's DCD
patch) with init steps skipped. A skipped step's `bl` becomes `mov w0, wzr`: the caller sees
success.

  patch.py STEP...   steps: sources voltage dfs target npa
  patch.py voltage   what lemonadep builds with

Why (2026-10-22): under this UEFI the 9 Pro's panel (command mode, amb670yf01) kept ABL's last
frame. Stripes painted between drivers showed it stop the moment ClockDxe starts, once with a
frame cut in half; no display clock, PLL, clock source, power switch, SMMU or pin register
differs before/after ClockDxe. With Clock_InitVoltage skipped the panel follows the framebuffer
(Silicium logo, the loader's text). That step takes over the rails of ClockRailConfig
(/vcs/vdd_cx, gx, mm, mx at 0x100); the display block is on vdd_mm (MMCX). Without it the rails
keep the bootloader's votes, which is what the display needs; nothing in this UEFI scales them.
"""
import struct, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '../../../../../Binaries/lemonade/QcomPkg/Drivers/ClockDxe/ClockDxe.patched.efi')
# call site (file offset = RVA here) -> the bl that is there in the OnePlus 9 binary
STEPS = {
    'sources': (0x6294, 0x940003d6),   # Clock_InitSourcesSoft  (bl 0x71ec)
    'voltage': (0x62c0, 0x940008e9),   # Clock_InitVoltage      (bl 0x8664)
    'dfs':     (0x62f4, 0x97fff371),   # Clock_InitDFSDomains   (bl 0x30b8, per domain)
    'target':  (0x6310, 0x9400201e),   # Clock_InitTarget       (bl 0xe388)
    'npa':     (0x6324, 0x97fffdc8),   # Clock_InitNPA          (bl 0x5a44)
}
MOV_W0_WZR = 0x2a1f03e0

d = bytearray(open(SRC, 'rb').read())
for step in sys.argv[1:]:
    off, insn = STEPS[step]
    assert struct.unpack_from('<I', d, off)[0] == insn, step
    struct.pack_into('<I', d, off, MOV_W0_WZR)
open(os.path.join(HERE, 'ClockDxe.efi'), 'wb').write(d)
print('ClockDxe.efi: skipped', ', '.join(sys.argv[1:]) or 'nothing')
