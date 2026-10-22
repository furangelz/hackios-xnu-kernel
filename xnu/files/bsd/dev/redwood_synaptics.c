/*
 * The touch screen of the OnePlus 9 Pro (lemonadep, SM8350): a Synaptics S3908 at address 0x4b on
 * the I2C bus of the SoC's serial engine 4, as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with board=lemonadep (xnuloader says it) and boot-arg touchscreen=1:
 * every address below belongs to that phone.
 *
 * From the phone's own device tree (its dtbo, overlay "Lahaina MTP lemonadep", node synaptics@4B
 * under qupv3_se4_i2c):
 *   bus        gpio20 (data) and gpio21 (clock), function "qup4" when the serial engine has them
 *   interrupt  gpio23, low = the chip has a message
 *   reset      gpio22, low = in reset
 *   supplies   vdd_2v8: LDO3 of the PM8350C ("ldoc3"), 3.008 V; enable2v8 gpio31, enable1v8 gpio197
 *              (gpio197 is an "eGPIO": the audio island's until claimed, see gpio_configure; until
 *              it is, its pad stays low, the chip's I/O supply off, and the bus and the interrupt
 *              line are held low by the unpowered chip)
 *   size       1440 x 3216 (touchpanel,panel-coords), the same as the panel's
 * Seen 2026-10-22: part "s3908-15.0.0", TouchComm 1, application firmware; max x 1439, max y 3215.
 * There is no pin, regulator or I2C framework in this kernel. The bus is driven by hand: open
 * drain, a line is pulled low by making its pin an output at 0 and let go by making it an input
 * with its pull-up; the clock line is waited for when let go (the chip may hold it low).
 *
 * The chip speaks Synaptics TouchComm, version 1. The host writes a command: its code, a 16-bit
 * length and the payload. The chip answers with messages: 0xa5, a status or report code, a 16-bit
 * length, the payload, 0x5a; a message longer than one read is fetched by "continued reads" that
 * start with 0xa5 0x03. After a reset the chip sends an identify report (0x10) by itself; touches
 * come as touch reports (0x11), whose layout the chip describes when asked (a list of fields and
 * their widths in bits, read least significant bit first). NOTHING THAT WRITES THE CHIP'S FLASH OR
 * ITS CONFIGURATION IS EVER SENT: identify, application info, touch report layout and enabling the
 * touch report are the only commands.
 *
 * Steps, each said in the log before it is taken (boot-arg touch_steps=N stops after the first N):
 *   1  read how the six pins are set up, and where RPMh keeps ldoc3          (reads only)
 *   2  take the pins; with boot-arg touch_power=1 also switch the supplies on (RPMh and two pins),
 *      looking at the lines after each
 *   3  reset the chip and read the first message (the identify report)
 *   4  ask for the application info and the touch report layout
 *   5  watch the interrupt line, 250 times a second, and turn reports into touches (the default)
 * Boot-arg touch_flip: bit 0 mirrors x, bit 1 mirrors y.
 *
 * Protocol: Synaptics' TouchComm drivers for Android (synaptics_tcm: tcm_core, touch report
 * parsing); RPMh, the command database and the pins as in bsd/dev/redwood_goodix.c (Linux
 * drivers/soc/qcom/{rpmh-rsc,cmd-db}.c, drivers/pinctrl/qcom/pinctrl-msm.c).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well (as in redwood_buttons.c): the kernel's
 * message buffer is a ring, and by the end of a run what was said at boot is gone from it.
 */
extern void loader_log_putc(char c);

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

static vm_offset_t tlmm, rsc, cmd_db;

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

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* a busy wait, for the bus's half periods (the timer counts at 19.2 MHz: 52 ns a tick) */
static void
spin_ns(uint32_t ns)
{
	uint64_t ticks, deadline;

	nanoseconds_to_absolutetime(ns, &ticks);
	deadline = mach_absolute_time() + (ticks ? ticks : 1);
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_OUTPUT              (1u << 9)
#define CFG_EGPIO_PRESENT       (1u << 11)      /* the pad can belong to the audio island's own pin controller ... */
#define CFG_EGPIO_ENABLE        (1u << 12)      /* ... unless this says it is ours (Linux pinctrl-msm.c) */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_SDA                 20
#define PIN_SCL                 21
#define PIN_RESET               22      /* low = in reset */
#define PIN_INT                 23      /* low = the chip has a message */
#define PIN_ENABLE_2V8          31      /* high = on */
#define PIN_ENABLE_1V8          197     /* high = on */

/*
 * A pin that can be the audio island's ("eGPIO", bit 11 set: gpio197, the touch's 1.8 V enable,
 * is one) answers to this controller only with bit 12 set; without it a write here changes the
 * register and not the pad (seen: gpio197 driven high read back low, in SEC as under the kernel).
 * Linux claims such a pin the same way whenever it gives it a function.
 */
static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	uint32_t egpio = (rd(GPIO_CFG(pin)) & CFG_EGPIO_PRESENT) ? CFG_EGPIO_PRESENT | CFG_EGPIO_ENABLE : 0;

	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? CFG_OUTPUT : 0) | egpio);
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

static void
say_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("synaptics: gpio%u (%s): function %u, %s, pull %u, %u mA%s; reads %u%s\n", pin, what, cfg >> 2 & 0xf,
	    (cfg & CFG_OUTPUT) ? "output" : "input", cfg & 3, ((cfg >> 6 & 7) + 1) * 2,
	    !(cfg & CFG_EGPIO_PRESENT) ? "" : (cfg & CFG_EGPIO_ENABLE) ? ", eGPIO claimed" : ", eGPIO NOT claimed (the island's)",
	    io & 1, (cfg & CFG_OUTPUT) ? ((io & 2) ? ", drives 1" : ", drives 0") : "");
}

/* ---- the supply (RPMh) --------------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7
#define SUPPLY_MV               3008            /* the device tree's vdd_2v8_volt */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("synaptics: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("synaptics: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done (as redwood_goodix.c). */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("synaptics: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* vdd_2v8: LDO3 of the PM8350C at the device tree's 3.008 V (the enable pins: power_sequence below). */
static bool
ldoc3_on(uint32_t ldo)
{
	uint32_t address[3] = { ldo + VRM_VOLTAGE, ldo + VRM_ENABLE, ldo + VRM_MODE };
	uint32_t data[3] = { SUPPLY_MV, 1, VRM_MODE_LDO_HPM };

	printf("synaptics: RPMh: ldoc3 at %#x to %u mV, on; slot status %#x, control %#x\n", ldo, SUPPLY_MV,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)));
	return rpmh_write(address, data, 3);
}

/*
 * Does GPIO 197's pad follow this controller? As an input with its pull-up it reads 1 when it does.
 * (It read 0 until the pin was claimed from the audio island: see gpio_configure.)
 */
static bool
gpio197_bank_powered(const char *when)
{
	bool level;

	gpio_configure(PIN_ENABLE_1V8, 0, PULL_UP, 2, false);
	sleep_ms(1);
	level = gpio_get(PIN_ENABLE_1V8);
	printf("synaptics: gpio197 as an input with its pull-up, %s: reads %u (%s)\n", when, level,
	    level ? "the pad follows" : "the pad does NOT follow: still the island's, or held low");
	return level;
}

/*
 * What a PM8350C regulator really is (slave 2, LDO n = peripheral 0x40 + n - 1), read from the PMIC:
 * status 0x08 (bit 7: ready), 0x40/0x41 the set voltage in mV, 0x45 mode, 0x46 bit 7 enabled.
 */
static void
say_ldo(unsigned n)
{
	uint16_t id = (uint16_t)(0x200 | (0x40 + n - 1));
	uint8_t status = 0, v[8] = { 0 };
	const char *problem = redwood_spmi_peek(id, 0x08, &status, 1);

	if (problem == NULL) {
		problem = redwood_spmi_peek(id, 0x40, v, 8);
	}
	if (problem != NULL) {
		printf("synaptics: PM8350C LDO%u: %s\n", n, problem);
		return;
	}
	printf("synaptics: PM8350C LDO%u: %s, %s, %u mV (status %#x, mode %#x, enable %#x)\n", n,
	    (v[6] & 0x80) ? "enabled" : "not enabled", (status & 0x80) ? "ready" : "not ready", v[0] | v[1] << 8, status,
	    v[5], v[6]);
}

/* the chip's three open-drain lines: all high once its I/O supply is up (they are pulled up to it) */
static bool
say_lines(const char *when)
{
	bool data = gpio_get(PIN_SDA), clock = gpio_get(PIN_SCL), irq = gpio_get(PIN_INT);

	printf("synaptics: %s: data %u, clock %u, interrupt %u\n", when, data, clock, irq);
	return data && clock;
}

/*
 * The supplies, a step at a time, with the lines looked at after each: the bus and the interrupt
 * line read low for as long as the chip's I/O supply is off (an unpowered chip clamps them).
 * The device tree marks both enable pins active-low, the vendor's driver drives them high: high
 * first, then low if the bus stayed down, and back to high if neither did anything.
 */
static void
power_sequence(uint32_t ldo)
{
	say_ldo(3);
	if (ldo == 0 || (ldo >> 16 & 0xf) != 4) {
		printf("synaptics: ldoc3 is not a regulator in RPMh (%#x): left alone\n", ldo);
	} else if (!ldoc3_on(ldo)) {
		printf("synaptics: the RPMh request for ldoc3 did not complete\n");
	}
	sleep_ms(5);
	say_ldo(3);
	if (say_lines("ldoc3 on")) {
		return;
	}
	gpio197_bank_powered("claimed");
	gpio_output(PIN_ENABLE_1V8, true, PULL_NONE, 2);
	sleep_ms(5);
	if (say_lines("and gpio197 high")) {
		return;
	}
	gpio_output(PIN_ENABLE_2V8, true, PULL_NONE, 2);
	sleep_ms(5);
	if (say_lines("and gpio31 high")) {
		return;
	}
	say_pin(PIN_ENABLE_2V8, "2.8 V enable");
	say_pin(PIN_ENABLE_1V8, "1.8 V enable");
	gpio_set(PIN_ENABLE_1V8, false);
	sleep_ms(5);
	if (say_lines("gpio197 low instead")) {
		return;
	}
	gpio_set(PIN_ENABLE_2V8, false);
	sleep_ms(5);
	if (say_lines("and gpio31 low")) {
		return;
	}
	gpio_set(PIN_ENABLE_1V8, true);
	gpio_set(PIN_ENABLE_2V8, true);
	sleep_ms(5);
	say_lines("both high again (as the vendor's driver leaves them)");
}

/* ---- I2C by hand ------------------------------------------------------------------------------- */

#define I2C_HALF_NS             2500    /* about 200 kHz */
#define I2C_STRETCH_US          2000    /* how long the chip may hold the clock low */

static uint64_t i2c_nacks, i2c_stretch_timeouts;

static void
line_low(unsigned pin)
{
	gpio_configure(pin, 0, PULL_UP, 2, true);       /* the output level is 0 (set once in i2c_take) */
}

static void
line_release(unsigned pin)
{
	gpio_configure(pin, 0, PULL_UP, 2, false);
}

static bool
scl_release(void)
{
	line_release(PIN_SCL);
	for (int i = 0; !gpio_get(PIN_SCL); i++) {
		if (i >= I2C_STRETCH_US) {
			i2c_stretch_timeouts++;
			return false;
		}
		spin_ns(1000);
	}
	return true;
}

static void
i2c_start(void)
{
	line_release(PIN_SDA);
	scl_release();
	spin_ns(I2C_HALF_NS);
	line_low(PIN_SDA);
	spin_ns(I2C_HALF_NS);
	line_low(PIN_SCL);
	spin_ns(I2C_HALF_NS);
}

static void
i2c_stop(void)
{
	line_low(PIN_SDA);
	spin_ns(I2C_HALF_NS);
	scl_release();
	spin_ns(I2C_HALF_NS);
	line_release(PIN_SDA);
	spin_ns(I2C_HALF_NS);
}

/* one byte out; true if the chip acknowledged it */
static bool
i2c_put(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		if (byte >> bit & 1) {
			line_release(PIN_SDA);
		} else {
			line_low(PIN_SDA);
		}
		spin_ns(I2C_HALF_NS);
		scl_release();
		spin_ns(I2C_HALF_NS);
		line_low(PIN_SCL);
	}
	line_release(PIN_SDA);
	spin_ns(I2C_HALF_NS);
	scl_release();
	spin_ns(I2C_HALF_NS);
	ack = !gpio_get(PIN_SDA);
	line_low(PIN_SCL);
	spin_ns(I2C_HALF_NS);
	return ack;
}

static uint8_t
i2c_get(bool ack)
{
	uint8_t byte = 0;

	line_release(PIN_SDA);
	for (int bit = 7; bit >= 0; bit--) {
		spin_ns(I2C_HALF_NS);
		scl_release();
		spin_ns(I2C_HALF_NS);
		byte = (uint8_t)(byte << 1 | gpio_get(PIN_SDA));
		line_low(PIN_SCL);
	}
	if (ack) {
		line_low(PIN_SDA);
	}
	spin_ns(I2C_HALF_NS);
	scl_release();
	spin_ns(I2C_HALF_NS);
	line_low(PIN_SCL);
	line_release(PIN_SDA);
	return byte;
}

#define TCM_ADDRESS             0x4b

static bool
i2c_read(uint8_t *data, uint32_t length)
{
	i2c_start();
	if (!i2c_put(TCM_ADDRESS << 1 | 1)) {
		i2c_stop();
		i2c_nacks++;
		return false;
	}
	for (uint32_t i = 0; i < length; i++) {
		data[i] = i2c_get(i + 1 < length);
	}
	i2c_stop();
	return true;
}

static bool
i2c_write(const uint8_t *data, uint32_t length)
{
	bool ok;

	i2c_start();
	ok = i2c_put(TCM_ADDRESS << 1);
	for (uint32_t i = 0; ok && i < length; i++) {
		ok = i2c_put(data[i]);
	}
	i2c_stop();
	if (!ok) {
		i2c_nacks++;
	}
	return ok;
}

/* The bus pins as plain GPIOs at level 0 (driven only when made outputs); free a stuck data line. */
static void
i2c_take(void)
{
	gpio_set(PIN_SDA, false);
	gpio_set(PIN_SCL, false);
	line_release(PIN_SDA);
	line_release(PIN_SCL);
	sleep_us(10);
	for (int i = 0; i < 9 && !gpio_get(PIN_SDA); i++) {
		line_low(PIN_SCL);
		spin_ns(5000);
		scl_release();
		spin_ns(5000);
	}
	i2c_stop();
}

/* ---- TouchComm ----------------------------------------------------------------------------------- */

#define TCM_MARKER              0xa5
#define TCM_PADDING             0x5a
#define TCM_STATUS_IDLE         0x00
#define TCM_STATUS_OK           0x01
#define TCM_STATUS_BUSY         0x02
#define TCM_STATUS_CONTINUED    0x03
#define TCM_STATUS_ERROR        0x0f
#define TCM_REPORT_IDENTIFY     0x10
#define TCM_REPORT_TOUCH        0x11
#define TCM_CMD_IDENTIFY        0x02
#define TCM_CMD_ENABLE_REPORT   0x05
#define TCM_CMD_GET_APP_INFO    0x20
#define TCM_CMD_GET_TOUCH_REPORT_CONFIG 0x25
#define TCM_MODE_APPLICATION    0x01
#define TCM_MAX_PAYLOAD         1024
#define TCM_CHUNK               256     /* bytes of payload fetched by one continued read */

static uint8_t tcm_payload[TCM_MAX_PAYLOAD + 1];   /* the payload and the end marker */
static uint8_t tcm_chunk[2 + TCM_CHUNK];
static uint8_t report_config[256];
static uint32_t report_config_size;
static uint32_t max_x = 1439, max_y = 3215, max_objects = 10;
static uint64_t messages, bad_messages;

/*
 * One message from the chip: its code and payload length (0, false: nothing could be read).
 * Status 0 (idle) means the chip had nothing to say.
 */
static bool
tcm_read_message(uint8_t *code, uint32_t *length)
{
	uint8_t header[4];
	uint32_t got = 0, total;

	if (!i2c_read(header, sizeof(header))) {
		return false;
	}
	if (header[0] != TCM_MARKER) {
		if (bad_messages++ < 8) {
			printf("synaptics: a message without its marker: %02x %02x %02x %02x\n", header[0], header[1], header[2],
			    header[3]);
		}
		return false;
	}
	*code = header[1];
	*length = header[2] | (uint32_t)header[3] << 8;
	if (*code == TCM_STATUS_IDLE || *code == TCM_STATUS_BUSY) {
		*length = 0;
		return true;
	}
	if (*length > TCM_MAX_PAYLOAD) {
		if (bad_messages++ < 8) {
			printf("synaptics: message %#x says %u bytes, more than %u\n", *code, *length, TCM_MAX_PAYLOAD);
		}
		return false;
	}
	/* the payload and the end marker, through continued reads (each starts with 0xa5 0x03) */
	total = *length + 1;
	while (got < total) {
		uint32_t part = total - got < TCM_CHUNK ? total - got : TCM_CHUNK;

		if (!i2c_read(tcm_chunk, part + 2)) {
			return false;
		}
		if (tcm_chunk[0] != TCM_MARKER || tcm_chunk[1] != TCM_STATUS_CONTINUED) {
			if (bad_messages++ < 8) {
				printf("synaptics: continued read of message %#x starts %02x %02x\n", *code, tcm_chunk[0],
				    tcm_chunk[1]);
			}
			return false;
		}
		memcpy(tcm_payload + got, tcm_chunk + 2, part);
		got += part;
	}
	if (tcm_payload[*length] != TCM_PADDING) {
		if (bad_messages++ < 8) {
			printf("synaptics: message %#x (%u bytes) ends with %#x, not 0x5a\n", *code, *length,
			    tcm_payload[*length]);
		}
		return false;
	}
	messages++;
	return true;
}

static void
say_identify(uint32_t length)
{
	char part[17] = { 0 };

	if (length < 24) {
		printf("synaptics: identify report of %u bytes, too short\n", length);
		return;
	}
	memcpy(part, tcm_payload + 2, 16);
	for (int i = 0; i < 16; i++) {
		if (part[i] != 0 && (part[i] < 32 || part[i] > 126)) {
			part[i] = '.';
		}
	}
	printf("synaptics: identify: protocol version %u, mode %#x (%s), part \"%s\", build %u, max write %u bytes\n",
	    tcm_payload[0], tcm_payload[1], tcm_payload[1] == TCM_MODE_APPLICATION ? "application firmware" : "NOT the application",
	    part, tcm_payload[18] | tcm_payload[19] << 8 | tcm_payload[20] << 16 | (uint32_t)tcm_payload[21] << 24,
	    tcm_payload[22] | tcm_payload[23] << 8);
}

/*
 * Send a command and wait for its status (reports that come meanwhile are dropped).
 * Returns the payload length of the answer, or -1.
 */
static int
tcm_command(uint8_t command, const uint8_t *payload, uint32_t length)
{
	uint8_t out[8];
	uint8_t code;
	uint32_t answer;

	if (length > sizeof(out) - 3) {
		return -1;
	}
	out[0] = command;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	memcpy(out + 3, payload, length);
	if (!i2c_write(out, 3 + length)) {
		printf("synaptics: command %#x: not acknowledged\n", command);
		return -1;
	}
	for (int i = 0; i < 500; i++) {
		sleep_ms(2);
		if (gpio_get(PIN_INT)) {
			continue;
		}
		if (!tcm_read_message(&code, &answer)) {
			continue;
		}
		if (code == TCM_STATUS_OK) {
			return (int)answer;
		}
		if (code > TCM_STATUS_CONTINUED && code <= TCM_STATUS_ERROR) {
			printf("synaptics: command %#x: status %#x\n", command, code);
			return -1;
		}
		if (code == TCM_REPORT_IDENTIFY) {
			say_identify(answer);
		}
	}
	printf("synaptics: command %#x: no answer in a second\n", command);
	return -1;
}

/* ---- touch reports ------------------------------------------------------------------------------- */

/* the fields of a touch report's layout */
#define TOUCH_END                       0x00
#define TOUCH_FOREACH_ACTIVE_OBJECT     0x01
#define TOUCH_FOREACH_OBJECT            0x02
#define TOUCH_FOREACH_END               0x03
#define TOUCH_PAD_TO_NEXT_BYTE          0x04
#define TOUCH_OBJECT_N_INDEX            0x06
#define TOUCH_OBJECT_N_CLASSIFICATION   0x07
#define TOUCH_OBJECT_N_X_POSITION       0x08
#define TOUCH_OBJECT_N_Y_POSITION       0x09
#define TOUCH_NUM_OF_ACTIVE_OBJECTS     0x18
#define CLASS_NONE                      0
#define CLASS_FINGER                    1
#define CLASS_GLOVED_FINGER             2

#define MAX_OBJECTS                     10

static struct object {
	uint32_t classification, x, y;
	bool seen;              /* in this report */
	bool down;              /* posted as down and not yet as up */
} objects[MAX_OBJECTS];
static uint32_t flip;
static uint64_t touch_reports, downs;

static uint32_t
bits_at(const uint8_t *data, uint32_t size, uint32_t offset, uint32_t bits)
{
	uint32_t value = 0;

	for (uint32_t i = 0; i < bits && i < 32; i++) {
		uint32_t at = offset + i;

		if (at / 8 >= size) {
			break;
		}
		value |= (uint32_t)(data[at / 8] >> (at % 8) & 1) << i;
	}
	return value;
}

static void
parse_touch_report(const uint8_t *data, uint32_t size)
{
	uint32_t idx = 0, offset = 0, next = 0, obj = 0, active = 0, done_objects = 0;
	bool active_only = false, active_known = false;

	for (unsigned i = 0; i < MAX_OBJECTS; i++) {
		objects[i].seen = false;
	}
	while (idx < report_config_size) {
		uint8_t field = report_config[idx++];
		uint32_t bits, value;

		switch (field) {
		case TOUCH_END:
			idx = report_config_size;
			break;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			next = idx;
			active_only = true;
			done_objects = 0;
			break;
		case TOUCH_FOREACH_OBJECT:
			obj = 0;
			next = idx;
			active_only = false;
			break;
		case TOUCH_FOREACH_END:
			/*
			 * Another object while the count says so; without a count, while the report has bits
			 * left (an object past its end reads as zeros: classification "none"). As Synaptics' own
			 * parser does.
			 */
			if (active_only) {
				done_objects++;
				if (active_known ? done_objects < active : offset < size * 8) {
					idx = next;
				}
			} else if (++obj < max_objects && obj < MAX_OBJECTS) {
				idx = next;
			}
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			offset = (offset + 7) / 8 * 8;
			break;
		default:
			if (idx >= report_config_size) {
				return;
			}
			bits = report_config[idx++];
			value = bits_at(data, size, offset, bits);
			offset += bits;
			switch (field) {
			case TOUCH_OBJECT_N_INDEX:
				obj = value;
				break;
			case TOUCH_OBJECT_N_CLASSIFICATION:
				if (obj < MAX_OBJECTS) {
					objects[obj].classification = value;
					objects[obj].seen = true;
				}
				break;
			case TOUCH_OBJECT_N_X_POSITION:
				if (obj < MAX_OBJECTS) {
					objects[obj].x = value;
				}
				break;
			case TOUCH_OBJECT_N_Y_POSITION:
				if (obj < MAX_OBJECTS) {
					objects[obj].y = value;
				}
				break;
			case TOUCH_NUM_OF_ACTIVE_OBJECTS:
				active = value;
				active_known = true;
				break;
			}
			break;
		}
	}

	touch_reports++;
	for (unsigned i = 0; i < MAX_OBJECTS; i++) {
		struct object *o = &objects[i];
		bool touching = o->seen && (o->classification == CLASS_FINGER || o->classification == CLASS_GLOVED_FINGER);

		if (touching) {
			uint32_t x = o->x > max_x ? max_x : o->x, y = o->y > max_y ? max_y : o->y;

			x = (uint32_t)((uint64_t)x * 65535 / (max_x ? max_x : 1));
			y = (uint32_t)((uint64_t)y * 65535 / (max_y ? max_y : 1));
			if (flip & 1) {
				x = 65535 - x;
			}
			if (flip & 2) {
				y = 65535 - y;
			}
			redwood_touch_post(o->down ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, i, x, y);
			if (!o->down && downs++ < 20) {
				printf("synaptics: finger %u down at %u, %u (of %u, %u)\n", i, o->x, o->y, max_x, max_y);
			}
			o->down = true;
		} else if (o->down) {
			redwood_touch_post(REDWOOD_TOUCH_UP, i, 0, 0);
			o->down = false;
		}
	}
}

/* ---- the steps ----------------------------------------------------------------------------------- */

static void
say_bytes(const char *what, const uint8_t *data, uint32_t length)
{
	char line[3 * 64 + 1];
	uint32_t n = 0;

	for (uint32_t i = 0; i < length && i < 64; i++) {
		n += (uint32_t)snprintf(line + n, sizeof(line) - n, "%02x ", data[i]);
	}
	line[n] = 0;
	printf("synaptics: %s (%u bytes): %s%s\n", what, length, line, length > 64 ? "..." : "");
}

static void
synaptics_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, power = 0, poll_us = 4000, ldo;
	uint8_t code;
	uint32_t length;
	int answer;
	bool identified = false;

	PE_parse_boot_argn("touch_power", &power, sizeof(power));
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));

	printf("synaptics: step 1: the pins as the firmware left them, and where RPMh keeps ldoc3\n");
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	say_pin(PIN_SDA, "bus data");
	say_pin(PIN_SCL, "bus clock");
	say_pin(PIN_RESET, "reset");
	say_pin(PIN_INT, "interrupt");
	say_pin(PIN_ENABLE_2V8, "2.8 V enable");
	say_pin(PIN_ENABLE_1V8, "1.8 V enable");
	ldo = rpmh_address("ldoc3");
	printf("synaptics: RPMh: ldoc3 is at %#x\n", ldo);
	if (steps < 2) {
		goto out;
	}

	printf("synaptics: step 2: take the bus pins (as GPIOs), reset, interrupt; supplies %s\n",
	    power ? "on (touch_power=1)" : "left as they are (touch_power=1 switches them on)");
	/* the interrupt line as the device tree has it: an input, no pull (the board pulls it up) */
	gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
	i2c_take();
	say_lines("before any supply");
	say_ldo(1);
	say_ldo(8);
	if (power) {
		power_sequence(ldo);
	}
	i2c_take();
	say_lines("bus taken");
	if (steps < 3) {
		goto out;
	}

	printf("synaptics: step 3: reset the chip (gpio22 low for 10 ms) and read its first message\n");
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	sleep_ms(10);
	gpio_set(PIN_RESET, true);
	for (int i = 0; i < 100 && !identified; i++) {
		sleep_ms(10);
		if (gpio_get(PIN_INT)) {
			continue;
		}
		if (!tcm_read_message(&code, &length)) {
			continue;
		}
		if (code == TCM_REPORT_IDENTIFY) {
			say_identify(length);
			identified = true;
		} else {
			printf("synaptics: first message: code %#x, %u bytes\n", code, length);
		}
	}
	if (!identified) {
		printf("synaptics: nothing from the chip in a second (interrupt line %u, %llu reads not acknowledged); "
		    "asking it to identify itself\n", gpio_get(PIN_INT), i2c_nacks);
		answer = tcm_command(TCM_CMD_IDENTIFY, NULL, 0);
		if (answer < 0) {
			printf("synaptics: the chip does not answer at %#x: no touch screen%s\n", TCM_ADDRESS,
			    power ? "" : " (its supplies were left as they were: try touch_power=1)");
			goto out;
		}
		say_identify((uint32_t)answer);
	}
	if (steps < 4) {
		goto out;
	}

	printf("synaptics: step 4: the application info and the touch report layout\n");
	answer = tcm_command(TCM_CMD_GET_APP_INFO, NULL, 0);
	if (answer >= 38) {
		uint32_t x = tcm_payload[32] | tcm_payload[33] << 8, y = tcm_payload[34] | tcm_payload[35] << 8;
		uint32_t n = tcm_payload[36] | tcm_payload[37] << 8;

		printf("synaptics: application info: version %u, status %u, max x %u, max y %u, %u objects\n",
		    tcm_payload[0] | tcm_payload[1] << 8, tcm_payload[2] | tcm_payload[3] << 8, x, y, n);
		if (x > 0 && y > 0) {
			max_x = x;
			max_y = y;
		}
		if (n > 0) {
			max_objects = n;
		}
	} else if (answer >= 0) {
		say_bytes("application info, shorter than expected", tcm_payload, (uint32_t)answer);
	}
	answer = tcm_command(TCM_CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0);
	if (answer <= 0 || (uint32_t)answer > sizeof(report_config)) {
		printf("synaptics: no touch report layout (%d): no touch screen\n", answer);
		goto out;
	}
	report_config_size = (uint32_t)answer;
	memcpy(report_config, tcm_payload, report_config_size);
	say_bytes("touch report layout", report_config, report_config_size);
	{
		uint8_t report = TCM_REPORT_TOUCH;

		answer = tcm_command(TCM_CMD_ENABLE_REPORT, &report, 1);
		printf("synaptics: touch reports enabled (%d)\n", answer);
	}
	if (steps < 5) {
		goto out;
	}

	printf("synaptics: step 5: watching the interrupt line every %u us\n", poll_us);
	for (uint64_t tick = 1;; tick++) {
		for (int n = 0; n < 4 && !gpio_get(PIN_INT); n++) {
			if (!tcm_read_message(&code, &length)) {
				break;
			}
			if (code == TCM_REPORT_TOUCH) {
				parse_touch_report(tcm_payload, length);
			} else if (code == TCM_REPORT_IDENTIFY) {
				/* the chip has reset itself: say so; the layout stays */
				say_identify(length);
			}
		}
		if (tick % (60000000 / (poll_us ? poll_us : 1)) == 0) {
			printf("synaptics: %llu touch reports, %llu messages (%llu bad), %llu reads not acknowledged, "
			    "%llu clock waits too long\n", touch_reports, messages, bad_messages, i2c_nacks, i2c_stretch_timeouts);
		}
		sleep_us(poll_us);
	}
out:
	if (steps < 5) {
		printf("synaptics: stopping here (touch_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
redwood_synaptics_init(void)
{
	uint32_t enabled = 0, steps = 5;
	char board[32];
	thread_t thread;

	if (!PE_parse_boot_arg_str("board", board, sizeof(board)) || strcmp(board, "lemonadep") != 0) {
		return;
	}
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(synaptics_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
