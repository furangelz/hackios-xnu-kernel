/**
  Display state snapshots (XNU_DISPLAY_SNAPSHOT, lemonadep).

  The OnePlus 9 Pro's panel keeps showing the bootloader's last frame once this UEFI runs. To
  find the stage that stops it, the display's clocks, power switch, interfaces, control paths
  and source pipes, the SMMU's stream matching and the panel's pins are recorded at three
  points: slot 0 in SEC before the SMMU detach (the bootloader's state), slot 1 right after it,
  slot 2 in BDS after all drivers. The XNU loader hands the slots over USB ("oem dispsnap").

  Read only: the one thing written is the slot, in RAM at the end of the "XNU Payload" region.
  A register whose clock or power is off is not read (that would hang the bus) and not listed.
  Each slot: magic, slot number, pair count, skipped mask, then (address, value) pairs.

  Offsets: Linux drivers/clk/qcom/{gcc-sm8350,dispcc-sm8250}.c,
  drivers/gpu/drm/msm/disp/dpu1/{catalog/dpu_7_0_sm8350.h,dpu_hw_intf.c,dpu_hw_ctl.c,dpu_hw_sspp.c};
  panel pins from the lahaina device tree (amb670yf01: reset gpio 24, TE gpio 82).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _XNU_DISPLAY_SNAPSHOT_H_
#define _XNU_DISPLAY_SNAPSHOT_H_

#include <Library/IoLib.h>

#define XNU_SNAP_BASE       0xF3FE0000UL
#define XNU_SNAP_SLOT_SIZE  0x2000UL
#define XNU_SNAP_SLOTS      5          // 0, 1: SEC; 2: BDS; 3, 4: right before and after ClockDxe
#define XNU_SNAP_MAGIC      0x50414E53 // "SNAP"


#define XNU_SNAP_SKIP_DISPCC  BIT0     // the display clock controller has no bus clock
#define XNU_SNAP_SKIP_MDP     BIT1     // the display block is without power or clocks, or not read

#define XNU_SNAP_READ_MDP     0        // the display controller's registers (off: see XnuSnapClockOn)

#define SNAP_GCC     0x00100000UL
#define SNAP_DISPCC  0x0AF00000UL
#define SNAP_MDP     0x0AE01000UL
#define SNAP_SMMU    0x15000000UL
#define SNAP_TLMM    0x0F100000UL

#define SNAP_GCC_DISP_AHB_CBCR     0x27004
#define SNAP_DISPCC_MDSS_GDSCR     0x3000
#define SNAP_DISPCC_MDSS_AHB_CBCR  0x2080
#define SNAP_DISPCC_MDSS_MDP_CBCR  0x200C

// Display test: paint rows [Band * 150, Band * 150 + 150) of the bootloader's framebuffer
// (1080 x 2412, 4320 bytes a row) in one colour. Which bands reach the panel tells the last
// stage at which it still took new frames (BootShim paints band 0).
static inline VOID
XnuFbTestBand (
  IN UINT32 Band,
  IN UINT32 Color)
{
  UINTN Row = 0xE4D00000UL + Band * 150 * 4320;

  for (UINTN i = 0; i < 150 * 4320; i += 4) {
    MmioWrite32 (Row + i, Color);
  }
}

static inline VOID
XnuSnapRead (
  IN UINTN Slot,
  IN UINTN Address)
{
  UINT32 Count = MmioRead32 (Slot + 8);

  if (16 + 8 * (Count + 1) > XNU_SNAP_SLOT_SIZE) {
    return;
  }

  MmioWrite32 (Slot + 16 + 8 * Count, (UINT32)Address);
  MmioWrite32 (Slot + 20 + 8 * Count, MmioRead32 (Address));
  MmioWrite32 (Slot + 8, Count + 1);
}

// A branch clock is read through only when it reports itself running. "Asked for but off" is
// not enough: the first version took that for on and hung the phone (2026-10-22, in SEC or
// after the drivers, unknown which), so the display controller itself is not read at all now.
static inline BOOLEAN
XnuSnapClockOn (
  IN UINT32 Cbcr)
{
  return !(Cbcr & BIT31);
}

static inline VOID
XnuDisplaySnapshot (
  IN UINT32 SlotIndex)
{
  // GCC: the display clock controller's bus clock, the display's AXI ports, its XO
  STATIC CONST UINT32 Gcc[] = { 0x27004, 0x2700C, 0x27014, 0x2701C };
  // DISPCC: MDSS power switch; ahb, mdp, pclk0, byte0, byte0_intf, esc0, vsync, mdp_lut, rot,
  // non_gdsc_ahb, xo branch clocks
  STATIC CONST UINT32 Dispcc[] = { 0x3000, 0x2080, 0x200C, 0x2004, 0x2028, 0x202C, 0x2038, 0x2024, 0x201C, 0x2014, 0x4004, 0x605C };
  // DISPCC PLL0 and PLL1 (Lucid 5LPE): mode, L, cal L, user ctl (3), config ctl (3), status,
  // opmode, alpha -- only registers clk-alpha-pll.c names (a sweep of the block hung the phone)
  STATIC CONST UINT32 Pll[]     = { 0x0000, 0x1000 };
  STATIC CONST UINT32 PllReg[]  = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x30, 0x38, 0x40 };
  // DISPCC clock sources (RCG2): ahb, byte0, esc0, mdp, pclk0, rot, vsync -- cmd, cfg, m, n, d
  STATIC CONST UINT32 Rcg[]     = { 0x22A0, 0x2110, 0x2148, 0x20C8, 0x2098, 0x20E0, 0x20F8 };
  STATIC CONST UINT32 RcgReg[]  = { 0x00, 0x04, 0x08, 0x0C, 0x10 };
  // INTF_1 and INTF_2 (DSI 0 and 1): timing engine, config, frame/line counters, tear check
  // (vsync select, enable, sync config, height, wrcount, init, count, thresh, start, irqs,
  // line counts, autorefresh)
  STATIC CONST UINT32 Intf[]  = { 0x35000, 0x36000 };
  STATIC CONST UINT32 IntfReg[] = { 0x000, 0x004, 0x0A8, 0x0AC, 0x0B0, 0x280, 0x284, 0x288, 0x28C, 0x290, 0x294,
                                    0x298, 0x29C, 0x2A0, 0x2A4, 0x2A8, 0x2AC, 0x2B0, 0x2B4 };
  // CTL_0 and CTL_1: top, flush, start, active interfaces
  STATIC CONST UINT32 Ctl[]     = { 0x15000, 0x16000 };
  STATIC CONST UINT32 CtlReg[]  = { 0x014, 0x018, 0x01C, 0x0F4 };
  // VIG0-3 and DMA0-3 source pipes: size, address, stride, format
  STATIC CONST UINT32 Sspp[]    = { 0x4000, 0x6000, 0x8000, 0xA000, 0x24000, 0x26000, 0x28000, 0x2A000 };
  STATIC CONST UINT32 SsppReg[] = { 0x00, 0x14, 0x24, 0x30 };
  // SMMU context bank: SCTLR, TTBR0 low/high, TCR, MAIR0, FSR, FAR low/high, FSYNR0
  STATIC CONST UINT32 CbReg[]   = { 0x00, 0x20, 0x24, 0x30, 0x38, 0x58, 0x60, 0x64, 0x68 };
  // the panel's pins: regulator, reset, TE and the other mdp_vsync pin
  STATIC CONST UINT32 Gpio[]    = { 12, 24, 82, 83 };

  UINTN  Slot    = XNU_SNAP_BASE + SlotIndex * XNU_SNAP_SLOT_SIZE;
  UINT32 Skipped = 0;
  UINT32 Id0, Id1, Groups, Banks, PageSize;
  UINTN  i, j;

  if (SlotIndex >= XNU_SNAP_SLOTS) {
    return;
  }

  MmioWrite32 (Slot + 0, XNU_SNAP_MAGIC);
  MmioWrite32 (Slot + 4, SlotIndex);
  MmioWrite32 (Slot + 8, 0);

  for (i = 0; i < ARRAY_SIZE (Gcc); i++) {
    XnuSnapRead (Slot, SNAP_GCC + Gcc[i]);
  }

  if (!XnuSnapClockOn (MmioRead32 (SNAP_GCC + SNAP_GCC_DISP_AHB_CBCR))) {
    Skipped |= XNU_SNAP_SKIP_DISPCC | XNU_SNAP_SKIP_MDP;
  } else {
    for (i = 0; i < ARRAY_SIZE (Dispcc); i++) {
      XnuSnapRead (Slot, SNAP_DISPCC + Dispcc[i]);
    }

    for (i = 0; i < ARRAY_SIZE (Pll); i++) {
      for (j = 0; j < ARRAY_SIZE (PllReg); j++) {
        XnuSnapRead (Slot, SNAP_DISPCC + Pll[i] + PllReg[j]);
      }
    }

    for (i = 0; i < ARRAY_SIZE (Rcg); i++) {
      for (j = 0; j < ARRAY_SIZE (RcgReg); j++) {
        XnuSnapRead (Slot, SNAP_DISPCC + Rcg[i] + RcgReg[j]);
      }
    }

    if (!XNU_SNAP_READ_MDP ||
        !(MmioRead32 (SNAP_DISPCC + SNAP_DISPCC_MDSS_GDSCR) & BIT31) ||
        !XnuSnapClockOn (MmioRead32 (SNAP_DISPCC + SNAP_DISPCC_MDSS_AHB_CBCR)) ||
        !XnuSnapClockOn (MmioRead32 (SNAP_DISPCC + SNAP_DISPCC_MDSS_MDP_CBCR))) {
      Skipped |= XNU_SNAP_SKIP_MDP;
    } else {
      for (i = 0; i < ARRAY_SIZE (Intf); i++) {
        for (j = 0; j < ARRAY_SIZE (IntfReg); j++) {
          XnuSnapRead (Slot, SNAP_MDP + Intf[i] + IntfReg[j]);
        }
      }

      for (i = 0; i < ARRAY_SIZE (Ctl); i++) {
        for (j = 0; j < ARRAY_SIZE (CtlReg); j++) {
          XnuSnapRead (Slot, SNAP_MDP + Ctl[i] + CtlReg[j]);
        }
      }

      for (i = 0; i < ARRAY_SIZE (Sspp); i++) {
        for (j = 0; j < ARRAY_SIZE (SsppReg); j++) {
          XnuSnapRead (Slot, SNAP_MDP + Sspp[i] + SsppReg[j]);
        }
      }
    }
  }

  // SMMU: ID0/ID1, then each stream match group's SMR and S2CR, and each context bank's CBAR
  Id0      = MmioRead32 (SNAP_SMMU + 0x20);
  Id1      = MmioRead32 (SNAP_SMMU + 0x24);
  Groups   = MIN (Id0 & 0xFF, 128);
  Banks    = MIN (Id1 & 0xFF, 128);
  PageSize = (Id1 & BIT31) ? SIZE_64KB : SIZE_4KB;

  XnuSnapRead (Slot, SNAP_SMMU + 0x20);
  XnuSnapRead (Slot, SNAP_SMMU + 0x24);

  for (i = 0; i < Groups; i++) {
    XnuSnapRead (Slot, SNAP_SMMU + 0x800 + 4 * i);
    XnuSnapRead (Slot, SNAP_SMMU + 0xC00 + 4 * i);
  }

  for (i = 0; i < Banks; i++) {
    XnuSnapRead (Slot, SNAP_SMMU + PageSize + 4 * i);
  }

  // The context bank the display's stream group translates through (the bootloader's page
  // tables): control, page table base, translation control, attributes, fault status, fault
  // address and syndrome. Banks follow the global space: (NUMPAGE + n) pages from the base.
  for (i = 0; i < Groups; i++) {
    UINT32 Smr  = MmioRead32 (SNAP_SMMU + 0x800 + 4 * i);
    UINT32 S2cr = MmioRead32 (SNAP_SMMU + 0xC00 + 4 * i);
    UINT32 Mask = (Smr >> 16) & 0x7FFF;
    UINTN  Bank;

    if (!(Smr & BIT31) || ((S2cr >> 16) & 3) != 0 || (0x820 & ~Mask) != ((Smr & 0xFFFF) & ~Mask) || (S2cr & 0xFF) >= Banks) {
      continue;
    }

    Bank = SNAP_SMMU + ((1UL << (((Id1 >> 28) & 7) + 1)) + (S2cr & 0xFF)) * PageSize;
    for (j = 0; j < ARRAY_SIZE (CbReg); j++) {
      XnuSnapRead (Slot, Bank + CbReg[j]);
    }
  }

  for (i = 0; i < ARRAY_SIZE (Gpio); i++) {
    XnuSnapRead (Slot, SNAP_TLMM + 0x1000 * Gpio[i]);
    XnuSnapRead (Slot, SNAP_TLMM + 0x1000 * Gpio[i] + 4);
  }

  MmioWrite32 (Slot + 12, Skipped);
}

#endif /* _XNU_DISPLAY_SNAPSHOT_H_ */
