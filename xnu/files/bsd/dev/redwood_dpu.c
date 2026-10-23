/*
 * The display engine of the OnePlus 9 Pro (lemonadep, SM8350: Qualcomm's "DPU" 7.0), as the boot
 * loader leaves it: sending the framebuffer at 0xe4d00000 to the panel, a command-mode panel with
 * a tearing-effect line (gpio82), over DSI0. Only with board=lemonadep (xnuloader says it).
 *
 * Frames without tearing (MiniIOMobileFramebuffer.cpp): the engine sends the framebuffer to the
 * panel 120 times a second by itself (autorefresh, on every tearing-effect pulse), 2.8 ms a
 * transfer; a frame written into that memory while a transfer reads it reaches the panel half old,
 * half new. So frames are drawn into a second picture the engine is not reading and the two source
 * pipes are pointed at it with a flush, which the engine takes at the start of a transfer, never
 * in the middle of one (redwood_dpu_flip(), redwood_dpu_flipped()). Before a kernel goes back to
 * the loader, or panics, the pipes are pointed at the boot loader's framebuffer again
 * (redwood_dpu_restore()): that is the memory the text console and the loader draw into.
 * Boot-arg dpu_flip=0: none of this, frames are written into the framebuffer as before.
 *
 * Boot-arg dpu_probe=1: say how it is set up -- which source pipe reads the framebuffer, which
 * control path sends it, whether the engine sends frames by itself ("autorefresh") and whether it
 * waits for the panel's tearing-effect signal -- and watch its frame counters for a second. Reads
 * only, after the two clocks without which a register of the display block hangs the bus (seen on
 * this phone, 22 October): the display clock controller's bus clock in GCC, and the bus clock of
 * the display block's registers; each is turned on if it is off, nothing else is written.
 *
 * Addresses: the vendor's device tree (lahaina-sde-common.dtsi: mdp_phys 0xae00000, the blocks'
 * offsets) and Linux's catalog for this SoC (drivers/gpu/drm/msm/disp/dpu1/catalog/
 * dpu_7_0_sm8350.h: the same offsets from the engine at 0xae01000); register layouts from Linux's
 * dpu_hw_{sspp,ctl,intf,lm}.c; clocks from the vendor's dispcc-lahaina.c and gcc-lahaina.c.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

void redwood_dpu_init(void);
bool redwood_dpu_ready(void);
void redwood_dpu_flip(uint64_t phys);
bool redwood_dpu_flipped(void);
void redwood_dpu_restore(void);
void redwood_dpu_say(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the clocks in front of the registers ------------------------------------------------------- */

#define GCC_DISP_PAGE_PHYS      0x00127000UL    /* GCC 0x100000: its page of display clocks */
#define GCC_DISP_AHB_CBCR       0x004           /* (0x127004) gcc_disp_ahb_clk: the display clock controller's bus */
#define DISPCC_PHYS             0x0af00000UL
#define DISPCC_SIZE             0x10000UL
#define DISPCC_MDSS_MDP_CBCR    0x200c
#define DISPCC_MDSS_VSYNC_CBCR  0x2024
#define DISPCC_MDSS_AHB_CBCR    0x207c          /* disp_cc_mdss_ahb_clk: the bus of the display block's registers */
#define DISPCC_MDSS_GDSCR       0x3000          /* disp_cc_mdss_core_gdsc (dt: gdsc_base 0xaf03000) */
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

/* ---- the display block ------------------------------------------------------------------------- */

#define MDSS_PHYS               0x0ae00000UL
#define MDSS_SIZE               0x90000UL
#define MDSS_HW_VERSION         0x0
#define DPU                     0x1000          /* the engine; Linux's catalog counts from here */
#define CTL(n)                  (DPU + 0x15000 + 0x1000 * (n))
#define CTL_LAYER_LM0           0x000
#define CTL_TOP                 0x014
#define CTL_FLUSH               0x018
#define CTL_START               0x01c
#define CTL_MERGE_3D_ACTIVE     0x0e4
#define CTL_DSC_ACTIVE          0x0e8
#define CTL_INTF_ACTIVE         0x0f4
#define CTL_FETCH_PIPE_ACTIVE   0x0fc
#define CTL_INTF_FLUSH          0x110
#define CTLS                    6
#define SSPPS                   8
static const struct {
	const char *name;
	uint32_t base, flush;           /* (base from DPU; the pipe's bit in CTL_FLUSH) */
} sspp[SSPPS] = {
	{ "vig0", 0x4000, 1u << 0 }, { "vig1", 0x6000, 1u << 1 }, { "vig2", 0x8000, 1u << 2 }, { "vig3", 0xa000, 1u << 18 },
	{ "dma0", 0x24000, 1u << 11 }, { "dma1", 0x26000, 1u << 12 }, { "dma2", 0x28000, 1u << 24 }, { "dma3", 0x2a000, 1u << 25 },
};
#define SSPP_SRC_SIZE           0x00
#define SSPP_SRC_XY             0x08
#define SSPP_OUT_SIZE           0x0c
#define SSPP_OUT_XY             0x10
#define SSPP_SRC0_ADDR          0x14
#define SSPP_SRC_YSTRIDE0       0x24
#define SSPP_SRC_FORMAT         0x30
#define SSPP_SRC_OP_MODE        0x38
#define SSPP_SRC_ADDR_SW_STATUS 0x70
#define LM(n)                   (DPU + 0x44000 + 0x1000 * (n))
#define LM_OP_MODE              0x00
#define LM_OUT_SIZE             0x04
#define LMS                     6
#define INTF(n)                 (DPU + 0x34000 + 0x1000 * (n))  /* 1: DSI0, 2: DSI1 */
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_CONFIG             0x004
#define INTF_CONFIG2            0x060
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0
#define INTF_STATUS             0x26c
#define INTF_TEAR_MDP_VSYNC_SEL 0x280
#define INTF_TEAR_TEAR_CHECK_EN 0x284
#define INTF_TEAR_SYNC_CONFIG_VSYNC 0x288
#define INTF_TEAR_SYNC_CONFIG_HEIGHT 0x28c
#define INTF_TEAR_SYNC_WRCOUNT  0x290
#define INTF_TEAR_VSYNC_INIT_VAL 0x294
#define INTF_TEAR_INT_COUNT_VAL 0x298           /* the panel's read pointer: frame << 16 | line */
#define INTF_TEAR_SYNC_THRESH   0x29c
#define INTF_TEAR_START_POS     0x2a0
#define INTF_TEAR_RD_PTR_IRQ    0x2a4
#define INTF_TEAR_WR_PTR_IRQ    0x2a8
#define INTF_TEAR_OUT_LINE_COUNT 0x2ac
#define INTF_TEAR_LINE_COUNT    0x2b0           /* the engine's write pointer: the line being sent */
#define INTF_TEAR_AUTOREFRESH_CONFIG 0x2b4      /* bit 31 on, [15:0] every so many tearing-effect pulses */

static vm_offset_t gcc_disp, dispcc, mdss;

/*
 * The two clocks in front of the display block's registers, as Linux has them on for as long as
 * it drives the display: true when the registers can be read. The first is gated by the hardware
 * while unused (hwcg bit 1: it reads "off" until it is talked to), so only its enable is looked at.
 */
static bool
registers_reachable(void)
{
	uint32_t ahb;

	if (gcc_disp == 0) {
		gcc_disp = ml_io_map(GCC_DISP_PAGE_PHYS, 0x1000);
		dispcc = ml_io_map(DISPCC_PHYS, DISPCC_SIZE);
		mdss = ml_io_map(MDSS_PHYS, MDSS_SIZE);
	}
	if (!(rd(gcc_disp + GCC_DISP_AHB_CBCR) & CBCR_ENABLE)) {
		printf("dpu: gcc disp_ahb (0x127004) = %#x: turning it on\n", rd(gcc_disp + GCC_DISP_AHB_CBCR));
		wr(gcc_disp + GCC_DISP_AHB_CBCR, rd(gcc_disp + GCC_DISP_AHB_CBCR) | CBCR_ENABLE);
		delay(10 * 1000);
	}
	if (!(rd(dispcc + DISPCC_MDSS_GDSCR) & GDSC_POWER_ON)) {
		printf("dpu: the display block has no power (dispcc mdss_gdscr %#x): not touched\n", rd(dispcc + DISPCC_MDSS_GDSCR));
		return false;
	}
	ahb = rd(dispcc + DISPCC_MDSS_AHB_CBCR);
	if (ahb & CBCR_OFF) {
		printf("dpu: dispcc mdss_ahb (0xaf0207c) = %#x: turning it on\n", ahb);
		wr(dispcc + DISPCC_MDSS_AHB_CBCR, ahb | CBCR_ENABLE);
		for (int i = 0; i < 100 && (rd(dispcc + DISPCC_MDSS_AHB_CBCR) & CBCR_OFF); i++) {
			delay(100);
		}
		ahb = rd(dispcc + DISPCC_MDSS_AHB_CBCR);
		if (ahb & CBCR_OFF) {
			printf("dpu: the display block's register clock stays off (%#x): not touched\n", ahb);
			return false;
		}
	}
	return true;
}

static void
probe(void)
{
	uint32_t counts[2][2][2];
	uint64_t times[2];

	printf("dpu: clocks: gcc disp_ahb %#x; dispcc mdss_gdscr %#x, mdss_ahb %#x, mdss_mdp %#x, mdss_vsync %#x\n",
	    rd(gcc_disp + GCC_DISP_AHB_CBCR), rd(dispcc + DISPCC_MDSS_GDSCR), rd(dispcc + DISPCC_MDSS_AHB_CBCR),
	    rd(dispcc + DISPCC_MDSS_MDP_CBCR), rd(dispcc + DISPCC_MDSS_VSYNC_CBCR));
	printf("dpu: mdss hw version %#x\n", rd(mdss + MDSS_HW_VERSION));
	for (unsigned n = 0; n < CTLS; n++) {
		vm_offset_t c = mdss + CTL(n);

		printf("dpu: ctl%u: layer(lm0) %#x top %#x flush %#x start %#x intf_active %#x fetch_pipe_active %#x dsc_active %#x "
		    "merge_3d_active %#x intf_flush %#x\n", n, rd(c + CTL_LAYER_LM0), rd(c + CTL_TOP), rd(c + CTL_FLUSH),
		    rd(c + CTL_START), rd(c + CTL_INTF_ACTIVE), rd(c + CTL_FETCH_PIPE_ACTIVE), rd(c + CTL_DSC_ACTIVE),
		    rd(c + CTL_MERGE_3D_ACTIVE), rd(c + CTL_INTF_FLUSH));
	}
	for (unsigned i = 0; i < SSPPS; i++) {
		vm_offset_t s = mdss + DPU + sspp[i].base;

		printf("dpu: %s: src_size %#x src_xy %#x out_size %#x out_xy %#x src0_addr %#x ystride0 %#x format %#x op_mode %#x "
		    "addr_sw_status %#x\n", sspp[i].name, rd(s + SSPP_SRC_SIZE), rd(s + SSPP_SRC_XY), rd(s + SSPP_OUT_SIZE),
		    rd(s + SSPP_OUT_XY), rd(s + SSPP_SRC0_ADDR), rd(s + SSPP_SRC_YSTRIDE0), rd(s + SSPP_SRC_FORMAT),
		    rd(s + SSPP_SRC_OP_MODE), rd(s + SSPP_SRC_ADDR_SW_STATUS));
	}
	for (unsigned n = 0; n < LMS; n++) {
		printf("dpu: lm%u: op_mode %#x out_size %#x\n", n, rd(mdss + LM(n) + LM_OP_MODE), rd(mdss + LM(n) + LM_OUT_SIZE));
	}
	for (unsigned n = 1; n <= 2; n++) {
		vm_offset_t f = mdss + INTF(n);

		printf("dpu: intf%u: timing_engine_en %#x config %#x config2 %#x frame_count %#x line_count %#x status %#x\n", n,
		    rd(f + INTF_TIMING_ENGINE_EN), rd(f + INTF_CONFIG), rd(f + INTF_CONFIG2), rd(f + INTF_FRAME_COUNT),
		    rd(f + INTF_LINE_COUNT), rd(f + INTF_STATUS));
		printf("dpu: intf%u tear: vsync_sel %#x check_en %#x sync_config_vsync %#x sync_config_height %#x wrcount %#x "
		    "vsync_init %#x int_count %#x thresh %#x start_pos %#x rd_ptr_irq %#x wr_ptr_irq %#x out_line_count %#x "
		    "line_count %#x autorefresh %#x\n", n, rd(f + INTF_TEAR_MDP_VSYNC_SEL), rd(f + INTF_TEAR_TEAR_CHECK_EN),
		    rd(f + INTF_TEAR_SYNC_CONFIG_VSYNC), rd(f + INTF_TEAR_SYNC_CONFIG_HEIGHT), rd(f + INTF_TEAR_SYNC_WRCOUNT),
		    rd(f + INTF_TEAR_VSYNC_INIT_VAL), rd(f + INTF_TEAR_INT_COUNT_VAL), rd(f + INTF_TEAR_SYNC_THRESH),
		    rd(f + INTF_TEAR_START_POS), rd(f + INTF_TEAR_RD_PTR_IRQ), rd(f + INTF_TEAR_WR_PTR_IRQ),
		    rd(f + INTF_TEAR_OUT_LINE_COUNT), rd(f + INTF_TEAR_LINE_COUNT), rd(f + INTF_TEAR_AUTOREFRESH_CONFIG));
	}
	/* how fast the counters of the interface the panel is on go: twice, a second apart */
	for (int i = 0; i < 2; i++) {
		for (unsigned n = 1; n <= 2; n++) {
			counts[i][n - 1][0] = rd(mdss + INTF(n) + INTF_TEAR_INT_COUNT_VAL);
			counts[i][n - 1][1] = rd(mdss + INTF(n) + INTF_FRAME_COUNT);
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &times[i]);
		if (i == 0) {
			delay(1000 * 1000);
		}
	}
	for (unsigned n = 1; n <= 2; n++) {
		printf("dpu: intf%u in %llu ms: the panel's read pointer from frame %u line %u to frame %u line %u; the interface's "
		    "frames from %u to %u\n", n, (times[1] - times[0]) / 1000000, counts[0][n - 1][0] >> 16, counts[0][n - 1][0] & 0xffff,
		    counts[1][n - 1][0] >> 16, counts[1][n - 1][0] & 0xffff, counts[0][n - 1][1], counts[1][n - 1][1]);
	}
	/* where the engine's write pointer is, eight times a millisecond and a bit apart: a transfer, or none */
	for (int i = 0; i < 8; i++) {
		printf("dpu: intf1 write pointer at line %u, read pointer %#x\n", rd(mdss + INTF(1) + INTF_TEAR_LINE_COUNT) & 0xffff,
		    rd(mdss + INTF(1) + INTF_TEAR_INT_COUNT_VAL));
		delay(1300);
	}
}

/* ---- flips ------------------------------------------------------------------------------------- */

/* The framebuffer the boot loader left: vig0 reads its left half, vig1 its right, from the same address. */
#define FB_PHYS                 0xe4d00000UL
#define FLIP_PIPES              (1u << 0 | 1u << 1)     /* CTL_FLUSH bits of vig0 and vig1 */
#define AUTOREFRESH_ON          (1u << 31)

static bool flips_ok;                   /* redwood_dpu_ready() found the engine as described above */
static bool flipped_away;               /* the pipes have been pointed somewhere else than FB_PHYS */

/*
 * Is the engine as this file expects it -- the two pipes reading the boot loader's framebuffer for
 * control path 0, sending frames by itself on the tearing-effect signal? Said once, either way.
 */
bool
redwood_dpu_ready(void)
{
	static bool asked;
	char board[32];
	uint32_t want = 1, fetch, autorefresh, tear;

	if (asked) {
		return flips_ok;
	}
	asked = true;
	if (!PE_parse_boot_arg_str("board", board, sizeof(board)) || strcmp(board, "lemonadep") != 0) {
		return false;
	}
	if (PE_parse_boot_argn("dpu_flip", &want, sizeof(want)) && !want) {
		printf("dpu: dpu_flip=0: frames go straight into the framebuffer\n");
		return false;
	}
	if (!registers_reachable()) {
		return false;
	}
	fetch = rd(mdss + CTL(0) + CTL_FETCH_PIPE_ACTIVE);
	autorefresh = rd(mdss + INTF(1) + INTF_TEAR_AUTOREFRESH_CONFIG);
	tear = rd(mdss + INTF(1) + INTF_TEAR_TEAR_CHECK_EN);
	if (rd(mdss + DPU + sspp[0].base + SSPP_SRC0_ADDR) != FB_PHYS || rd(mdss + DPU + sspp[1].base + SSPP_SRC0_ADDR) != FB_PHYS ||
	    (fetch & 0x30000) != 0x30000 || !(autorefresh & AUTOREFRESH_ON) || !(tear & 1)) {
		printf("dpu: not as expected (vig0 %#x, vig1 %#x, ctl0 fetch_pipe_active %#x, intf1 autorefresh %#x, tear check %#x): "
		    "no flips\n", rd(mdss + DPU + sspp[0].base + SSPP_SRC0_ADDR), rd(mdss + DPU + sspp[1].base + SSPP_SRC0_ADDR),
		    fetch, autorefresh, tear);
		return false;
	}
	printf("dpu: vig0 and vig1 read the framebuffer for ctl0, which sends a frame on every tearing-effect pulse "
	    "(autorefresh %#x): frames can go to the panel by flips\n", autorefresh);
	flips_ok = true;
	return true;
}

/* Both pipes to read from `phys` (one picture: vig1's source rectangle is its right half), from the next transfer on. */
void
redwood_dpu_flip(uint64_t phys)
{
	if (!flips_ok) {
		return;
	}
	__asm__ volatile ("dsb sy" ::: "memory");       /* the picture in memory before the engine is told about it */
	wr(mdss + DPU + sspp[0].base + SSPP_SRC0_ADDR, (uint32_t)phys);
	wr(mdss + DPU + sspp[1].base + SSPP_SRC0_ADDR, (uint32_t)phys);
	wr(mdss + CTL(0) + CTL_FLUSH, FLIP_PIPES);
	flipped_away = phys != FB_PHYS;
}

/* Has the engine taken the last flip? It clears the flush bits at the start of the transfer that reads the new picture. */
bool
redwood_dpu_flipped(void)
{
	return !flips_ok || !(rd(mdss + CTL(0) + CTL_FLUSH) & FLIP_PIPES);
}

/* A line of the framebuffer driver's about flips, where the loader keeps it (the kernel's own log is a ring). */
void
redwood_dpu_say(const char *line)
{
	printf("%s", line);
}

/* The boot loader's framebuffer on the panel again: for the text console, a panic, the loader. */
void
redwood_dpu_restore(void)
{
	if (flips_ok && flipped_away) {
		redwood_dpu_flip(FB_PHYS);
	}
}

void
redwood_dpu_init(void)
{
	char board[32];
	uint32_t want = 0;

	if (!PE_parse_boot_arg_str("board", board, sizeof(board)) || strcmp(board, "lemonadep") != 0) {
		return;
	}
	if (PE_parse_boot_argn("dpu_probe", &want, sizeof(want)) && want) {
		printf("dpu: probe: the display engine as the boot loader left it\n");
		if (registers_reachable()) {
			probe();
		}
	}
}
