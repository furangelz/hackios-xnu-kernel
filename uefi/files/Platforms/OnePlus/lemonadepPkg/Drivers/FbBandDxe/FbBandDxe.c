/**
  Display test (lemonadep): paints band FB_BAND of the bootloader's framebuffer in FB_COLOR when
  it is started, a second after. Instances sit between groups of drivers in APRIORI.inc; the
  last band that reaches the panel names the group after which it stopped taking new frames.
  Seen: the panel stops the moment ClockDxe starts (band 7 complete, band 8 never shown); the
  cause was Clock_InitVoltage (Binaries/ClockDxe/patch.py). Not in the build now: to use it again,
  list FbBand<N>.inf in APRIORI.inc between drivers, in DXE.inc and in lemonadep.dsc.

  FB_DUMP = 0 (the instance before ClockDxe) / 1 (after): also record display snapshot slot
  3 / 4 (XnuDisplaySnapshot.h: clocks, PLLs, clock sources, SMMU, pins), as close to ClockDxe as
  each instance runs: after painting before it, first thing after it. Read only, into RAM in
  the "XNU Payload" region. (A raw 36 KB sweep of the clock controller hung the phone.)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/XnuDisplaySnapshot.h>

#ifdef FB_MMCX_TEST
#include <Protocol/EFIClock.h>
#endif

#ifdef FB_DUMP
//
// GCC's display page: before ClockDxe it is not in the memory space map yet. It is added for the
// read and removed again, or HWIO's own AddMemorySpace of all of GCC for ClockDxe fails on the
// overlap and the boot stops (seen once, 2026-10-22).
//
STATIC
VOID
Record ()
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_PHYSICAL_ADDRESS            Page  = SNAP_GCC + (SNAP_GCC_DISP_AHB_CBCR & ~0xFFF);
  BOOLEAN                         Added = FALSE;

  if (EFI_ERROR (gDS->GetMemorySpaceDescriptor (Page, &Desc))) {
    return;
  }

  if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    if (EFI_ERROR (gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Page, SIZE_4KB, EFI_MEMORY_UC))) {
      return;
    }

    Added = TRUE;
  }

  if (!EFI_ERROR (gDS->SetMemorySpaceAttributes (Page, SIZE_4KB, EFI_MEMORY_UC))) {
    XnuDisplaySnapshot (3 + FB_DUMP);
  }

  if (Added) {
    gDS->RemoveMemorySpace (Page, SIZE_4KB);
  }
}
#endif

#ifdef FB_MMCX_TEST
//
// Seen: ClockDxe changes no display clock, PLL, clock source or the power switch (slots 3/4 are
// the same), yet the panel stops as it starts. What it does own is the MMCX rail (/vcs/vdd_mm,
// ClockRailConfig) the display block runs on: a fresh vote that knows no display user would drop
// it. Telling ClockDxe that the display's power domain is in use makes it vote that rail again.
// Notes in slot 4: 0x10 LocateProtocol, 0x11 GetClockPowerDomainID, 0x12 the id, 0x13 Enable.
//
STATIC
VOID
DisplayDomainInUse ()
{
  EFI_CLOCK_PROTOCOL *Clock;
  EFI_STATUS          Status;
  UINTN               Id = 0;

  Status = gBS->LocateProtocol (&gEfiClockProtocolGuid, NULL, (VOID **)&Clock);
  XnuSnapNote (4, 0x10, (UINT32)Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = Clock->GetClockPowerDomainID (Clock, "disp_cc_mdss_core_gdsc", &Id);
  XnuSnapNote (4, 0x11, (UINT32)Status);
  XnuSnapNote (4, 0x12, (UINT32)Id);
  if (EFI_ERROR (Status)) {
    return;
  }

  XnuSnapNote (4, 0x13, (UINT32)Clock->EnableClockPowerDomain (Clock, Id));
}
#endif

EFI_STATUS
EFIAPI
FbBandEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
#if defined (FB_DUMP) && FB_DUMP == 1
  Record ();
#endif

#ifdef FB_MMCX_TEST
  DisplayDomainInUse ();
#endif

  gBS->Stall (1000000);
  XnuFbTestBand (FB_BAND, FB_COLOR);

#if defined (FB_DUMP) && FB_DUMP == 0
  Record ();
#endif

  return EFI_SUCCESS;
}
