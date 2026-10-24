/*
 * The battery of the OnePlus 9 Pro (lemonadep, SM8350): two cells in series and a TI BQ28Z610 fuel
 * gauge in front of them, on an I2C bus of the application processor. Only with board=lemonadep
 * and boot-arg battery=1 (redwood_battery_init() starts this instead of reading the loader's UEFI
 * cache). The device tree also puts an MPS MP2650 charger on another bus; on this phone nothing
 * answers at its address (seen 23 October, battery_scan=1: of that bus only the wireless charging's
 * pump, an HL7227 at 0x51). Wired charging is the audio DSP's battery manager's on this SoC
 * (redwood_adsp.c on the redwood), and without it the battery takes no charge: on USB the gauge
 * measured about 0 mA with the phone lightly loaded (23 October), and 300 to 500 mA out of the
 * battery with the GPU and every core at speed (24 October): the port gives what it can, the
 * battery the rest. That current is how "a charger is there" is told here, for want of a status
 * to read (see below), so a phone under load on USB counts as running from its battery.
 *
 * From the phone's device tree (dtbo overlay "lemonadep"):
 *   gauge    qupv3_se2_i2c, bq27541-battery@55 with qcom,batt_bq28z610, 100 kHz: gpio12 data, gpio13 clock
 *   charger  qupv3_se0_i2c, mp2650-charger@5c: gpio4 data, gpio5 clock
 * Both serial engines are GSI-only (lahaina-qupv3.dtsi: dmas = <&gpi_dma0 ...>), as the touch
 * screen's is, so both buses are driven by hand as redwood_synaptics.c drives its own: open drain,
 * a line pulled low by making its pin an output at 0, let go by making it an input with its pull-up.
 *
 * NOTHING IS WRITTEN TO EITHER CHIP: every access is a register read (the register's address
 * written, then a repeated start and the read). The gauge's standard commands and the charger's
 * status and ADC registers are OnePlus's drivers' (drivers/power/oplus/gauge_ic/oplus_bq27541.[ch],
 * charger_ic/oplus_mp2650.[ch] in LineageOS android_kernel_oneplus_sm8350). Whatever the charger
 * does, it does with the settings it has: its own defaults once its watchdog has run out (Android
 * is not there to kick it), and they are logged once at the start, register for register.
 *
 * Every five seconds what the gauge says goes to the power source iOS reads (redwood_battery_report(),
 * RedwoodBattery.cpp): the state of charge, the voltage of a cell (half the pack's: iOS knows
 * one-cell phones), the current (positive while charging), the temperature, the charges in mAh, the
 * cycle count. Charging: the charger's status where it answers; otherwise from the current alone --
 * more than 20 mA into the battery is charging, less than 50 mA out of it is a phone running from
 * its USB port (this kernel never draws so little), more is a phone running from its battery.
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
extern void loader_log_putc(char c);

void redwood_gauge_start(void);

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

static vm_offset_t tlmm;

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

/* a busy wait, for the buses' half periods */
static void
spin_ns(uint32_t ns)
{
	uint64_t ticks, deadline;

	nanoseconds_to_absolutetime(ns, &ticks);
	deadline = mach_absolute_time() + (ticks ? ticks : 1);
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- the two buses, by hand -------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_OUTPUT              (1u << 9)
#define CFG_EGPIO_PRESENT       (1u << 11)      /* (as in redwood_synaptics.c: a pin of the island's is claimed) */
#define CFG_EGPIO_ENABLE        (1u << 12)
#define PULL_UP                 3
#define BUS_MA                  4
#define HALF_NS                 5000            /* 100 kHz, the gauge's bus speed in the device tree */
#define STRETCH_US              20000           /* how long a chip may hold the clock low */

struct bus {
	const char *name;
	unsigned sda, scl;
	uint32_t keep_sda, keep_scl;    /* eGPIO bits to keep in the pins' configuration */
	uint64_t nacks, stretches;
};

static struct bus gauge_bus = { .name = "gauge", .sda = 12, .scl = 13 };
static struct bus charger_bus = { .name = "charger", .sda = 4, .scl = 5 };

static void
line(struct bus *b, unsigned pin, bool low)
{
	uint32_t keep = pin == b->sda ? b->keep_sda : b->keep_scl;

	wr(GPIO_CFG(pin), keep | PULL_UP | (BUS_MA / 2 - 1) << 6 | (low ? CFG_OUTPUT : 0));
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static bool
scl_release(struct bus *b)
{
	line(b, b->scl, false);
	for (int i = 0; !level(b->scl); i++) {
		if (i >= STRETCH_US) {
			b->stretches++;
			return false;
		}
		spin_ns(1000);
	}
	return true;
}

static void
bus_start(struct bus *b)
{
	line(b, b->sda, false);
	scl_release(b);
	spin_ns(HALF_NS);
	line(b, b->sda, true);
	spin_ns(HALF_NS);
	line(b, b->scl, true);
	spin_ns(HALF_NS);
}

static void
bus_stop(struct bus *b)
{
	line(b, b->sda, true);
	spin_ns(HALF_NS);
	scl_release(b);
	spin_ns(HALF_NS);
	line(b, b->sda, false);
	spin_ns(HALF_NS);
}

/* one byte out; true if it was acknowledged */
static bool
bus_put(struct bus *b, uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		line(b, b->sda, !(byte >> bit & 1));
		spin_ns(HALF_NS);
		scl_release(b);
		spin_ns(HALF_NS);
		line(b, b->scl, true);
	}
	line(b, b->sda, false);
	spin_ns(HALF_NS);
	scl_release(b);
	spin_ns(HALF_NS);
	ack = !level(b->sda);
	line(b, b->scl, true);
	spin_ns(HALF_NS);
	return ack;
}

static uint8_t
bus_get(struct bus *b, bool ack)
{
	uint8_t byte = 0;

	line(b, b->sda, false);
	for (int bit = 7; bit >= 0; bit--) {
		spin_ns(HALF_NS);
		scl_release(b);
		spin_ns(HALF_NS);
		byte = (uint8_t)(byte << 1 | level(b->sda));
		line(b, b->scl, true);
	}
	if (ack) {
		line(b, b->sda, true);
	}
	spin_ns(HALF_NS);
	scl_release(b);
	spin_ns(HALF_NS);
	line(b, b->scl, true);
	line(b, b->sda, false);
	return byte;
}

/* `count` bytes from register `reg` of the chip at `address`: the register written, a repeated start, the read */
static bool
bus_read(struct bus *b, uint8_t address, uint8_t reg, uint8_t *data, unsigned count)
{
	bool ok;

	bus_start(b);
	ok = bus_put(b, (uint8_t)(address << 1)) && bus_put(b, reg);
	if (ok) {
		line(b, b->sda, false);         /* the repeated start: data high, clock high, data low */
		spin_ns(HALF_NS);
		scl_release(b);
		spin_ns(HALF_NS);
		line(b, b->sda, true);
		spin_ns(HALF_NS);
		line(b, b->scl, true);
		spin_ns(HALF_NS);
		ok = bus_put(b, (uint8_t)(address << 1 | 1));
	}
	for (unsigned i = 0; ok && i < count; i++) {
		data[i] = bus_get(b, i + 1 < count);
	}
	bus_stop(b);
	if (!ok) {
		b->nacks++;
	}
	return ok;
}

/*
 * Boot-arg battery_scan=1: which addresses on a bus answer a one-byte read (the read of a chip's
 * current register: nothing changes), to see what is there and whether the bus works at all.
 */
static void
bus_scan(struct bus *b)
{
	char line[300];
	int n = 0;

	for (unsigned address = 0x03; address <= 0x77; address++) {
		uint8_t byte;
		bool ok;

		bus_start(b);
		ok = bus_put(b, (uint8_t)(address << 1 | 1));
		if (ok) {
			byte = bus_get(b, false);
		}
		bus_stop(b);
		if (ok && n < (int)sizeof(line) - 8) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %#x", address);
		}
	}
	line[n] = 0;
	printf("battery: %s bus: addresses that answer:%s\n", b->name, n ? line : " none");
}

/* The pins as plain GPIOs (an eGPIO claimed as such: see redwood_synaptics.c), at output level 0; a stuck data line freed. */
static void
bus_take(struct bus *b)
{
	uint32_t sda = rd(GPIO_CFG(b->sda)), scl = rd(GPIO_CFG(b->scl));

	printf("battery: %s bus: gpio%u (data) cfg %#x reads %u, gpio%u (clock) cfg %#x reads %u\n", b->name, b->sda, sda,
	    level(b->sda), b->scl, scl, level(b->scl));
	b->keep_sda = (sda & CFG_EGPIO_PRESENT) ? CFG_EGPIO_PRESENT | CFG_EGPIO_ENABLE : 0;
	b->keep_scl = (scl & CFG_EGPIO_PRESENT) ? CFG_EGPIO_PRESENT | CFG_EGPIO_ENABLE : 0;
	wr(GPIO_IO(b->sda), 0);
	wr(GPIO_IO(b->scl), 0);
	line(b, b->sda, false);
	line(b, b->scl, false);
	delay(10);
	for (int i = 0; i < 9 && !level(b->sda); i++) {
		line(b, b->scl, true);
		spin_ns(HALF_NS);
		scl_release(b);
		spin_ns(HALF_NS);
	}
	bus_stop(b);
}

/* ---- the gauge (BQ28Z610 at 0x55) ----------------------------------------------------------------- */

#define GAUGE_ADDRESS           0x55
#define GAUGE_TEMPERATURE       0x06            /* 0.1 K */
#define GAUGE_VOLTAGE           0x08            /* mV, the pack's (both cells) */
#define GAUGE_FLAGS             0x0a
#define GAUGE_CURRENT           0x0c            /* mA, now (positive: charging) */
#define GAUGE_REMAINING         0x10            /* mAh */
#define GAUGE_FULL              0x12            /* mAh, as the gauge has learnt it */
#define GAUGE_AVERAGE_CURRENT   0x14            /* mA */
#define GAUGE_CYCLES            0x2a
#define GAUGE_SOC               0x2c            /* % */
#define GAUGE_HEALTH            0x2e            /* % */
#define GAUGE_DESIGN            0x3c            /* mAh, as designed */

static bool
gauge_word(uint8_t reg, int32_t *value, bool is_signed)
{
	uint8_t data[2];

	if (!bus_read(&gauge_bus, GAUGE_ADDRESS, reg, data, 2)) {
		return false;
	}
	*value = is_signed ? (int16_t)(data[0] | data[1] << 8) : (int32_t)(data[0] | data[1] << 8);
	return true;
}

/* ---- the charger (MP2650 at 0x5c) ------------------------------------------------------------------ */

#define CHARGER_ADDRESS         0x5c
#define CHARGER_STATUS          0x13            /* bit 1: 0 = the input is good; bits 3:2: 0 none, 1 pre-, 2 fast charge, 3 done */
#define CHARGER_FAULTS          0x14            /* bit 7 watchdog, 5:4 charge fault, 3 battery overvoltage, 2:0 NTC */
#define CHARGER_INPUT_VOLTAGE   0x1c            /* [15:6] x 25 mV */
#define CHARGER_INPUT_CURRENT   0x1e            /* [15:6] x 6.25 mA */
#define CHARGER_CHARGE_CURRENT  0x1a            /* [15:6] x 12.5 mA */
#define CHARGER_BATTERY_VOLTAGE 0x16            /* [15:6] x 12.5 mV */

static bool
charger_byte(uint8_t reg, uint8_t *value)
{
	return bus_read(&charger_bus, CHARGER_ADDRESS, reg, value, 1);
}

static bool
charger_adc(uint8_t reg, uint32_t step_hundredths, int32_t *value)
{
	uint8_t data[2];

	if (!bus_read(&charger_bus, CHARGER_ADDRESS, reg, data, 2)) {
		return false;
	}
	*value = (int32_t)(((uint32_t)(data[0] | data[1] << 8) >> 6) * step_hundredths / 100);
	return true;
}

/* ---- the reading ------------------------------------------------------------------------------- */

static void
gauge_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct redwood_battery_state state;
	uint8_t registers[0x16];
	bool all = true, any = false, charging_seen = false, charging_stopped = false;

	uint32_t scan = 0;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	bus_take(&gauge_bus);
	bus_take(&charger_bus);
	if (PE_parse_boot_argn("battery_scan", &scan, sizeof(scan)) && scan) {
		bus_scan(&gauge_bus);
		bus_scan(&charger_bus);
	}
	/* the charger's settings as they are: its defaults, unless something has set it since the phone started */
	for (unsigned reg = 0; reg < sizeof(registers); reg++) {
		bool ok = charger_byte((uint8_t)reg, &registers[reg]);

		all = all && ok;
		any = any || ok;
	}
	if (any) {
		printf("battery: charger registers 0x00..0x15%s:", all ? "" : " (not all could be read)");
		for (unsigned reg = 0; reg < sizeof(registers); reg++) {
			printf(" %02x", registers[reg]);
		}
		printf("\n");
	} else {
		printf("battery: no charger answers at %#x: charging is told from the gauge's current\n", CHARGER_ADDRESS);
	}

	for (unsigned tick = 0;; tick++) {
		int32_t soc = -1, mv = 0, ma = 0, decikelvin = 0, flags = 0, remaining = 0, full = 0, design = 0, cycles = 0;
		int32_t vin = 0, iin = 0, ichg = 0, vbat = 0;
		uint8_t status = 0xff, faults = 0xff;
		bool gauge_ok, charger_ok;

		gauge_ok = gauge_word(GAUGE_SOC, &soc, false) && gauge_word(GAUGE_VOLTAGE, &mv, false) &&
		    gauge_word(GAUGE_AVERAGE_CURRENT, &ma, true) && gauge_word(GAUGE_TEMPERATURE, &decikelvin, false) &&
		    gauge_word(GAUGE_FLAGS, &flags, false);
		if (gauge_ok) {
			(void)gauge_word(GAUGE_REMAINING, &remaining, false);
			(void)gauge_word(GAUGE_FULL, &full, false);
			(void)gauge_word(GAUGE_CYCLES, &cycles, false);
			(void)gauge_word(GAUGE_DESIGN, &design, false);
		}
		charger_ok = any && charger_byte(CHARGER_STATUS, &status) && charger_byte(CHARGER_FAULTS, &faults);
		if (charger_ok) {
			(void)charger_adc(CHARGER_INPUT_VOLTAGE, 2500, &vin);
			(void)charger_adc(CHARGER_INPUT_CURRENT, 625, &iin);
			(void)charger_adc(CHARGER_CHARGE_CURRENT, 1250, &ichg);
			(void)charger_adc(CHARGER_BATTERY_VOLTAGE, 1250, &vbat);
		}
		/* the battery's current changing sides is said at once: charging begins or ends */
		if (gauge_ok && (ma > 20) != charging_seen) {
			charging_seen = ma > 20;
			printf("battery: %s: %d mA, %d%%, %d mV\n", charging_seen ? "the battery takes charge" : "the battery takes no charge",
			    ma, soc, mv);
		}
		if (tick % 12 == 0) {
			printf("battery: gauge %s: %d%%, %d mV, %d mA, %d.%d C, flags %#x, %d of %d mAh (design %d), %d cycles; charger %s: "
			    "status %#x faults %#x, in %d mV %d mA, charging %d mA, battery %d mV; %llu+%llu reads not acknowledged, "
			    "%llu+%llu clock waits too long\n", gauge_ok ? "ok" : "NOT ANSWERING", soc, mv, ma,
			    (decikelvin - 2731) / 10, ((decikelvin - 2731) % 10 + 10) % 10, flags, remaining, full, design, cycles,
			    charger_ok ? "ok" : "NOT ANSWERING", status, faults, vin, iin, ichg, vbat, gauge_bus.nacks, charger_bus.nacks,
			    gauge_bus.stretches, charger_bus.stretches);
		}
		/*
		 * The charging is the audio DSP's (adsp=1), and the DSP does not read this battery's
		 * temperature (it says 25 degrees, always) nor this gauge. So the gauge watches: charging
		 * outside 0..45 C, or a cell above 4.40 V (the DSP's own limit is 4.35: 8.7 V for the pack),
		 * and the DSP is shut down, which ends the charging without telling the charger anything.
		 */
		if (gauge_ok && ma > 20 && !charging_stopped) {
			int32_t celsius = (decikelvin - 2731) / 10;

			if (celsius > 45 || celsius < 0 || mv / 2 > 4400) {
				charging_stopped = true;
				printf("battery: CHARGING STOPPED: %d C, %d mV a cell while taking %d mA: the audio DSP, which charges, is "
				    "shut down\n", celsius, mv / 2, ma);
				redwood_adsp_stop();
			}
		}
		if (gauge_ok && soc >= 0 && soc <= 100) {
			int32_t usb_type = 0, usb_uv = 0, usb_ua = 0;
			int usb = redwood_battmgr_usb(&usb_type, &usb_uv, &usb_ua);
			/* plugged in: what the charger says if it answers, else what the DSP says, else the current alone */
			bool input = charger_ok ? !(status & (1u << 1)) : usb >= 0 ? usb != 0 : ma > -50;
			unsigned stage = charger_ok ? (status >> 2) & 3 : ma > 20 ? 2 : 0;

			bzero(&state, sizeof(state));
			state.size = sizeof(state);
			state.percent_hundredths = soc * 100;
			state.voltage_uv = mv * 1000 / 2;
			state.current_ua = ma * 1000;
			state.temperature_hundredths = (decikelvin - 2731) * 10;
			state.present = 1;
			state.external = input;
			state.status = !input ? 2 : stage == 3 || soc >= 100 ? 4 : stage != 0 ? 1 : 3;
			state.charge_uah = remaining * 1000;
			state.full_uah = full * 1000;
			state.design_uah = design * 1000;
			state.cycles = cycles;
			state.usb_type = usb > 0 ? usb_type : 0;
			state.usb_voltage_uv = !input ? 0 : charger_ok ? vin * 1000 : usb_uv;
			state.usb_current_ua = !input ? 0 : charger_ok ? iin * 1000 : usb_ua;
			redwood_battery_report(&state);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
redwood_gauge_start(void)
{
	thread_t thread;

	printf("battery: lemonadep: the BQ28Z610 gauge (gpio12/13) and the MP2650 charger (gpio4/5), read every 5 s\n");
	if (kernel_thread_start(gauge_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
