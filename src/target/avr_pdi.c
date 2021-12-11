#include "general.h"
#include "target_probe.h"
#include "target_internal.h"
#include "avr.h"
#include "exception.h"

#define IR_PDI		0x7U
#define PDI_BREAK	0xBBU
#define PDI_DELAY	0xDBU
#define PDI_EMPTY	0xEBU

#define PDI_LDCS	0x80U
#define PDI_STCS	0xC0U

void avr_pdi_init(avr_pdi_t *pdi)
{
	/* Check for a valid part number in the IDCode */
	if ((pdi->idcode & 0x0FFFF000) == 0) {
		DEBUG_WARN("Invalid PDI idcode %08" PRIx32 "\n", pdi->idcode);
		free(pdi);
		return false;
	}
	DEBUG_INFO("AVR ID 0x%08" PRIx32 " (v%d)\n", pdi->idcode,
		(uint8_t)((pdi->idcode >> 28U) & 0xfU));
	jtag_dev_write_ir(&jtag_proc, pdi->dp_jd_index, IR_PDI);
	return true;
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

static bool avr_dev_shift_dr(jtag_proc_t *jp, uint8_t jd_index, uint8_t *dout, const uint8_t din)
{
	jtag_dev_t *d = &jtag_devs[jd_index];
	uint8_t result = 0;
	uint16_t request = 0, response = 0;
	uint8_t *data = (uint8_t *)&request;
	if (!dout)
		return false;
	jtagtap_shift_dr();
	jp->jtagtap_tdi_seq(0, ones, d->dr_prescan);
	data[0] = din;
	// Calculate the parity bit
	for (uint8_t i = 0; i < 8; ++i)
		data[1] ^= (din >> i) & 1U;
	jp->jtagtap_tdi_tdo_seq((uint8_t *)&response, 1, (uint8_t *)&request, 9);
	jp->jtagtap_tdi_seq(1, ones, d->dr_postscan);
	jtagtap_return_idle();
	data = (uint8_t *)&response;
	// Calculate the parity bit
	for (uint8_t i = 0; i < 8; ++i)
		result ^= (data[0] >> i) & 1U;
	*dout = data[0];
	return result == data[1];
}

bool avr_pdi_reg_write(avr_pdi_t *pdi, uint8_t reg, uint8_t value)
{
	uint8_t result = 0, command = PDI_STCS | reg;
	if (reg >= 16 ||
		avr_dev_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		avr_dev_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, value))
		return false;
	return result == PDI_EMPTY;
}

uint8_t avr_pdi_reg_read(avr_pdi_t *pdi, uint8_t reg)
{
	uint8_t result = 0, command = PDI_LDCS | reg;
	if (reg >= 16 ||
		avr_dev_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command) ||
		result != PDI_EMPTY ||
		!avr_dev_shift_dr(&jtag_proc, pdi->dp_jd_index, &result, command))
		return 0xFFU; // TODO - figure out a better way to indicate failure.
	return result;
}
