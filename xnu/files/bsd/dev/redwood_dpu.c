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
 * The panel's brightness (redwood_dpu_backlight(), for MiniIOMobileFramebuffer.cpp's backlight;
 * boot-arg backlight=1): the DCS command 0x51 to the panel through DSI0. See below.
 *
 * Addresses: the vendor's device tree (lahaina-sde-common.dtsi: mdp_phys 0xae00000, the blocks'
 * offsets) and Linux's catalog for this SoC (drivers/gpu/drm/msm/disp/dpu1/catalog/
 * dpu_7_0_sm8350.h: the same offsets from the engine at 0xae01000); register layouts from Linux's
 * dpu_hw_{sspp,ctl,intf,lm}.c; clocks from the vendor's dispcc-lahaina.c and gcc-lahaina.c.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void MiniIOMobileFramebufferBacklight(void);    /* iokit/Kernel/MiniIOMobileFramebuffer.cpp: iOS's backlight service */

void redwood_dpu_init(void);
bool redwood_dpu_ready(void);
void redwood_dpu_flip(uint64_t phys);
bool redwood_dpu_flipped(void);
void redwood_dpu_restore(void);
void redwood_dpu_say(const char *line);
bool redwood_dpu_backlight_ready(void);
int redwood_dpu_backlight(uint32_t level);

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

/* ---- the panel's brightness -------------------------------------------------------------------- */

/*
 * The panel (Samsung AMB670YF01, on DSI0) takes its brightness as the DCS command 0x51 with a
 * 12-bit level, high byte first: 1..0x7ff, and above that its high-brightness range, which is not
 * used here (the vendor's dtbo: bl_ctrl_dcs, bl-min-level 1, bl-normal-max-level 0x7ff,
 * brightness-default-level 0x190, where the boot loader leaves it).
 *
 * DSI0's command engine sends it while the display engine goes on sending frames: the controller
 * puts a command packet between the packets of a frame if it must (the interleave setting in
 * DSI_COMMAND_MODE_MDP_CTRL), and here it is started when no frame is being sent (DSI_STATUS),
 * if that comes within 20 ms. The packet goes through the FIFO of the controller's test-pattern
 * block, as the vendor's dsi_ctrl_hw_cmn_kickoff_fifo_command() does it: nothing in memory for the
 * controller to fetch, so nothing the display's SMMU has to let it read. In high-speed mode, as
 * commands go while the display is on. Registers: the vendor's dsi_ctrl_reg.h (techpack/display/
 * msm/dsi); the packet as its dsi_ctrl_copy_and_pad_cmd() lays one out: word count, data type,
 * flags (last packet, long packet), the bytes, 0xff up to a whole word.
 *
 * Boot-arg backlight=1 (MiniIOMobileFramebuffer.cpp gives iOS a backlight then). Boot-arg
 * backlight_test=1: at boot, the panel dim (0x40), bright (0x7ff), then as it was, 1.5 s each.
 */
#define DSI0_PHYS                       0x0ae94000UL
#define DSI_SIZE                        0x400
#define DSI_HW_VERSION                  0x000
#define DSI_CTRL                        0x004
#define DSI_STATUS                      0x008
#define DSI_COMMAND_MODE_DMA_CTRL       0x03c
#define DSI_COMMAND_MODE_MDP_CTRL       0x040
#define DSI_DMA_CMD_LENGTH              0x04c
#define DSI_ACK_ERR_STATUS              0x068
#define DSI_TRIG_CTRL                   0x084
#define DSI_CMD_MODE_DMA_SW_TRIGGER     0x090
#define DSI_LANE_STATUS                 0x0a8
#define DSI_INT_CTRL                    0x110
#define DSI_CLK_CTRL                    0x11c
#define DSI_TEST_PATTERN_GEN_CTRL       0x15c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x17c
#define DSI_TPG_DMA_FIFO_STATUS         0x1dc
#define DSI_TPG_DMA_FIFO_RESET          0x1ec
#define DSI_CTRL_ENABLE                 (1u << 0)
#define DSI_CTRL_CMD_MODE               (1u << 2)
#define DSI_STATUS_DMA_BUSY             (1u << 1)
#define DSI_STATUS_MDP_BUSY             (1u << 2)
#define DMA_CTRL_LOW_POWER              (1u << 26)
#define DMA_CTRL_EMBEDDED               (1u << 28)
#define DMA_CTRL_BROADCAST              (3u << 30)
#define TRIG_DMA                        0xfu            /* DSI_TRIG_CTRL: what starts a command: 4, the software */
#define TRIG_DMA_SW                     0x4u
#define INT_CMD_DMA_DONE                (1u << 0)
#define INT_ERROR                       (1u << 24)
#define TPG_CMD_DMA_FIFO                (1u << 1 | 1u << 2 | 3u << 16)  /* the command engine takes its packet from the FIFO */
#define DCS_SET_DISPLAY_BRIGHTNESS      0x51
#define BACKLIGHT_MAX                   0x7ff
#define BACKLIGHT_BOOT                  0x190

static vm_offset_t dsi;
static bool backlight_ok;
static uint32_t backlight_level = BACKLIGHT_BOOT;       /* what the panel was last told */
static uint32_t backlight_sent, backlight_failed;
static uint32_t backlight_lowest = BACKLIGHT_MAX, backlight_highest;     /* of the levels sent */
static LCK_GRP_DECLARE(backlight_lock_group, "redwood-backlight");
static LCK_MTX_DECLARE(backlight_lock, &backlight_lock_group);

static void
dsi_fifo_reset(void)
{
	wr(dsi + DSI_TEST_PATTERN_GEN_CTRL, 0);
	wr(dsi + DSI_TPG_DMA_FIFO_RESET, 1);
	delay(1);
	wr(dsi + DSI_TPG_DMA_FIFO_RESET, 0);
}

/* 0x51 with `level` to the panel; 0 once the controller says the packet is out, else what went wrong. */
static int
dcs_brightness(uint32_t level, uint32_t *waited_us)
{
	uint64_t began = mach_absolute_time(), now;
	unsigned i;
	int done;

	for (i = 0; i < 400 && (rd(dsi + DSI_STATUS) & (DSI_STATUS_MDP_BUSY | DSI_STATUS_DMA_BUSY)); i++) {
		delay(50);
	}
	if (rd(dsi + DSI_STATUS) & DSI_STATUS_DMA_BUSY) {
		return EBUSY;                   /* the last command is still going: not two at once */
	}
	dsi_fifo_reset();
	wr(dsi + DSI_INT_CTRL, (rd(dsi + DSI_INT_CTRL) & ~INT_ERROR) | INT_CMD_DMA_DONE);       /* an old "done" away */
	wr(dsi + DSI_TEST_PATTERN_GEN_CTRL, TPG_CMD_DMA_FIFO);
	wr(dsi + DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0xc0u << 24 | 0x39u << 16 | 3u);        /* 03 00 39 c0: 3 bytes, DCS long write, last */
	wr(dsi + DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0xffu << 24 | (level & 0xff) << 16 | (level >> 8 & 0xff) << 8 |
	    DCS_SET_DISPLAY_BRIGHTNESS);
	wr(dsi + DSI_COMMAND_MODE_DMA_CTRL, (rd(dsi + DSI_COMMAND_MODE_DMA_CTRL) & ~(DMA_CTRL_LOW_POWER | DMA_CTRL_BROADCAST)) |
	    DMA_CTRL_EMBEDDED);
	wr(dsi + DSI_DMA_CMD_LENGTH, 8);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(dsi + DSI_CMD_MODE_DMA_SW_TRIGGER, 1);
	for (i = 0; i < 2000 && !(rd(dsi + DSI_INT_CTRL) & INT_CMD_DMA_DONE); i++) {
		delay(10);
	}
	done = rd(dsi + DSI_INT_CTRL) & INT_CMD_DMA_DONE;
	if (done) {
		wr(dsi + DSI_INT_CTRL, (rd(dsi + DSI_INT_CTRL) & ~INT_ERROR) | INT_CMD_DMA_DONE);
	}
	dsi_fifo_reset();                       /* as the vendor leaves the FIFO after a command */
	now = mach_absolute_time();
	if (waited_us != NULL) {
		uint64_t ns;

		absolutetime_to_nanoseconds(now - began, &ns);
		*waited_us = (uint32_t)(ns / 1000);
	}
	return done ? 0 : ETIMEDOUT;
}

/* Can the panel's brightness be set (board=lemonadep, backlight=1, DSI0 sending frames in command mode)? */
bool
redwood_dpu_backlight_ready(void)
{
	return backlight_ok;
}

/* The panel's brightness: 0 (dark) .. 0x7ff, as DCS 0x51 has it. 0, or why it was not set. */
int
redwood_dpu_backlight(uint32_t level)
{
	uint32_t waited = 0;
	int error;

	if (!backlight_ok) {
		return ENXIO;
	}
	if (level > BACKLIGHT_MAX) {
		level = BACKLIGHT_MAX;
	}
	lck_mtx_lock(&backlight_lock);
	if (level == backlight_level && backlight_sent != 0) {
		lck_mtx_unlock(&backlight_lock);
		return 0;
	}
	error = dcs_brightness(level, &waited);
	if (error == 0) {
		backlight_sent++;
		backlight_lowest = level < backlight_lowest ? level : backlight_lowest;
		backlight_highest = level > backlight_highest ? level : backlight_highest;
		if (backlight_sent <= 8 || backlight_sent % 50 == 0) {
			printf("dpu: backlight %#x -> %#x (%u us; %u sent, %u failed)\n", backlight_level, level, waited,
			    backlight_sent, backlight_failed);
		}
		backlight_level = level;
	} else if (backlight_failed++ < 8) {
		printf("dpu: backlight %#x not sent (error %d after %u us): dsi status %#x int_ctrl %#x ack_err %#x lane_status %#x "
		    "tpg_fifo %#x\n", level, error, waited, rd(dsi + DSI_STATUS), rd(dsi + DSI_INT_CTRL), rd(dsi + DSI_ACK_ERR_STATUS),
		    rd(dsi + DSI_LANE_STATUS), rd(dsi + DSI_TPG_DMA_FIFO_STATUS));
	}
	lck_mtx_unlock(&backlight_lock);
	return error;
}

static void
dsi_registers(const char *when)
{
	printf("dpu: dsi0 %s: hw_version %#x ctrl %#x status %#x dma_ctrl %#x mdp_ctrl %#x trig_ctrl %#x int_ctrl %#x clk_ctrl %#x "
	    "lane_status %#x ack_err %#x tpg_ctrl %#x tpg_fifo %#x\n", when, rd(dsi + DSI_HW_VERSION), rd(dsi + DSI_CTRL),
	    rd(dsi + DSI_STATUS), rd(dsi + DSI_COMMAND_MODE_DMA_CTRL), rd(dsi + DSI_COMMAND_MODE_MDP_CTRL), rd(dsi + DSI_TRIG_CTRL),
	    rd(dsi + DSI_INT_CTRL), rd(dsi + DSI_CLK_CTRL), rd(dsi + DSI_LANE_STATUS), rd(dsi + DSI_ACK_ERR_STATUS),
	    rd(dsi + DSI_TEST_PATTERN_GEN_CTRL), rd(dsi + DSI_TPG_DMA_FIFO_STATUS));
}

static void
backlight_init(void)
{
	uint32_t want = 0, ctrl, trig;
	static const uint32_t test[] = { 0x40, BACKLIGHT_MAX, BACKLIGHT_BOOT };

	if (!PE_parse_boot_argn("backlight", &want, sizeof(want)) || !want || !registers_reachable()) {
		return;
	}
	dsi = ml_io_map(DSI0_PHYS, DSI_SIZE);
	dsi_registers("as found");
	ctrl = rd(dsi + DSI_CTRL);
	if ((ctrl & (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE)) != (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE)) {
		printf("dpu: dsi0 is not on in command mode (ctrl %#x): no backlight\n", ctrl);
		return;
	}
	trig = rd(dsi + DSI_TRIG_CTRL);
	if ((trig & TRIG_DMA) != TRIG_DMA_SW) {
		printf("dpu: dsi0 commands start on %#x, not by software: set\n", trig & TRIG_DMA);
		wr(dsi + DSI_TRIG_CTRL, (trig & ~TRIG_DMA) | TRIG_DMA_SW);
	}
	backlight_ok = true;
	printf("dpu: the panel's brightness goes by DCS 0x51 through dsi0 (from %#x, the boot loader's)\n", backlight_level);
	if (PE_parse_boot_argn("backlight_test", &want, sizeof(want)) && want) {
		for (unsigned i = 0; i < sizeof(test) / sizeof(test[0]); i++) {
			printf("dpu: backlight_test: %#x\n", test[i]);
			redwood_dpu_backlight(test[i]);
			delay(1500 * 1000);
		}
		dsi_registers("after the test");
	}
	MiniIOMobileFramebufferBacklight();
}

/* The boot loader's framebuffer on the panel again: for the text console, a panic, the loader. */
void
redwood_dpu_restore(void)
{
	if (flips_ok && flipped_away) {
		redwood_dpu_flip(FB_PHYS);
	}
	/* and the boot loader's brightness (without the lock: this may be a panic, every other processor stopped) */
	if (backlight_ok) {
		printf("dpu: backlight: %u levels sent, %u not, the last %#x (%#x..%#x seen)\n", backlight_sent, backlight_failed,
		    backlight_level, backlight_lowest, backlight_highest);
	}
	if (backlight_ok && backlight_level != BACKLIGHT_BOOT && dcs_brightness(BACKLIGHT_BOOT, NULL) == 0) {
		backlight_level = BACKLIGHT_BOOT;
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
	backlight_init();
}
