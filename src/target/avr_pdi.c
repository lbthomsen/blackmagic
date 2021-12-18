#include "general.h"
#include "target_probe.h"
#include "target_internal.h"
#include "avr.h"
#include "exception.h"
#include "gdb_packet.h"

#define IR_PDI		0x7U
#define IR_BYPASS	0xFU

#define PDI_BREAK	0xBBU
#define PDI_DELAY	0xDBU
#define PDI_EMPTY	0xEBU

#define PDI_LDCS	0x80U
#define PDI_STCS	0xC0U

static enum target_halt_reason avr_halt_poll(target *t, target_addr_t *watch);

bool avr_pdi_init(avr_pdi_t *pdi)
{
	target *t;

	/* Check for a valid part number in the IDCode */
	if ((pdi->idcode & 0x0FFFF000) == 0) {
		DEBUG_WARN("Invalid PDI idcode %08" PRIx32 "\n", pdi->idcode);
		free(pdi);
		return false;
	}
	DEBUG_INFO("AVR ID 0x%08" PRIx32 " (v%d)\n", pdi->idcode,
		(uint8_t)((pdi->idcode >> 28U) & 0xfU));
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_BYPASS);

	t = target_new();
	if (!t)
		return false;

	t->cpuid = pdi->idcode;
	t->part_id = (pdi->idcode >> 12) & 0xFFFFU;
	t->driver = "Atmel AVR";
	t->core = "AVR";
	t->priv = pdi;
	t->priv_free = free;

	t->attach = avr_attach;
	t->detach = avr_detach;
	t->halt_poll = avr_halt_poll;

	if (atxmega_probe(t))
		return true;
	pdi->halt_reason = TARGET_HALT_RUNNING;
	return true;
}

bool avr_pdi_reg_write(avr_pdi_t *pdi, uint8_t reg, uint8_t value)
{
	uint8_t result = 0, command = PDI_STCS | reg;
	if (reg >= 16 ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, value))
		return false;
	return result == PDI_EMPTY;
}

uint8_t avr_pdi_reg_read(avr_pdi_t *pdi, uint8_t reg)
{
	uint8_t result = 0, command = PDI_LDCS | reg;
	if (reg >= 16 ||
		avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		!avr_jtag_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command))
		return 0xFFU; // TODO - figure out a better way to indicate failure.
	return result;
}

void avr_add_flash(target *t, uint32_t start, size_t length)
{
	struct target_flash *f = calloc(1, sizeof(*f));
	if (!f) {			/* calloc failed: heap exhaustion */
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}

	f->start = start;
	f->length = length;
	f->blocksize = 0x100;
	f->erased = 0xff;
	target_add_flash(t, f);
}

bool avr_attach(target *t)
{
	avr_pdi_t *pdi = t->priv;
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_PDI);

	return true;
}

void avr_detach(target *t)
{
	avr_pdi_t *pdi = t->priv;
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_BYPASS);
}

static enum target_halt_reason avr_halt_poll(target *t, target_addr_t *watch)
{
	avr_pdi_t *pdi = t->priv;
	(void)watch;
	return pdi->halt_reason;
}
