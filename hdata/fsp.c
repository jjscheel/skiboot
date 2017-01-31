/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <device.h>
#include "spira.h"
#include <cpu.h>
#include <vpd.h>
#include <ccan/str/str.h>
#include <interrupts.h>
#include <inttypes.h>

#include "hdata.h"

enum sp_type {
	SP_BAD = 0,
	SP_UNKNOWN,
	SP_FSP,
	SP_BMC,
};

static const char * const sp_names[] = {
	"Broken", "Unknown", "FSP", "BMC",
};

static enum sp_type find_service_proc_type(const struct HDIF_common_hdr *spss,
		int index)
{
	const struct spss_sp_impl *sp_impl;
	int hw_ver, sw_ver, flags;
	enum sp_type sp_type;
	bool functional, installed;

	/* Find an check the SP Implementation structure */
	sp_impl = HDIF_get_idata(spss, SPSS_IDATA_SP_IMPL, NULL);
	if (!CHECK_SPPTR(sp_impl)) {
		prerror("SP #%d: SPSS/SP_Implementation not found !\n", index);
		return SP_BAD;
	}

	hw_ver = be16_to_cpu(sp_impl->hw_version);
	sw_ver = be16_to_cpu(sp_impl->sw_version);
	flags  = be16_to_cpu(sp_impl->func_flags);

	switch (hw_ver) {
	case 0x2: /* We only support FSP2 */
		sp_type = SP_FSP;
		break;
	case 0x3:
		sp_type = SP_BMC;
		break;
	default:
		sp_type = SP_UNKNOWN;
	}

	if (sp_type == SP_UNKNOWN)
		return SP_UNKNOWN;

	installed  = !!(flags & SPSS_SP_IMPL_FLAGS_INSTALLED);
	functional = !!(flags & SPSS_SP_IMPL_FLAGS_FUNCTIONAL);

	if (!installed || !functional) {
		prerror("%s #%d not usable: %sinstalled, %sfunctional\n",
			sp_names[sp_type], index,
			installed  ? "" : "not ",
			functional ? "" : "not ");

		return SP_BAD;
	}

	prlog(PR_INFO, "%s #%d: HW version %d, SW version %d, chip DD%d.%d\n",
	      sp_names[sp_type], index, hw_ver, sw_ver,
	      sp_impl->chip_version >> 4,
	      sp_impl->chip_version & 0xf);

	return sp_type;
}

/*
 * Note on DT representation of the PSI links and FSPs:
 *
 * We create a XSCOM node for each PSI host bridge(one per chip),
 *
 * This is done in spira.c
 *
 * We do not create the /psi MMIO variant at this stage, it will
 * be added by the psi driver in skiboot.
 *
 * We do not put the FSP(s) as children of these. Instead, we create
 * a top-level /fsps node with the FSPs as children.
 *
 * Each FSP then has a "links" property which is an array of chip IDs
 */

static struct dt_node *fsp_create_node(const void *spss, int i,
				       struct dt_node *parent)
{
	const struct spss_sp_impl *sp_impl;
	struct dt_node *node;

	sp_impl = HDIF_get_idata(spss, SPSS_IDATA_SP_IMPL, NULL);

	node = dt_new_addr(parent, "fsp", i);
	assert(node);

	dt_add_property_cells(node, "reg", i);

	if (be16_to_cpu(sp_impl->hw_version) == 1) {
		dt_add_property_strings(node, "compatible", "ibm,fsp",
				"ibm,fsp1");
		/* Offset into the FSP MMIO space where the mailbox
		 * registers are */
		/* seen in the FSP1 spec */
		dt_add_property_cells(node, "reg-offset", 0xb0016000);
	} else if (be16_to_cpu(sp_impl->hw_version) == 2) {
		dt_add_property_strings(node, "compatible", "ibm,fsp",
				"ibm,fsp2");
		dt_add_property_cells(node, "reg-offset", 0xb0011000);
	}
	dt_add_property_cells(node, "hw-version", be16_to_cpu(sp_impl->hw_version));
	dt_add_property_cells(node, "sw-version", be16_to_cpu(sp_impl->sw_version));

	if (be16_to_cpu(sp_impl->func_flags) & SPSS_SP_IMPL_FLAGS_PRIMARY)
		dt_add_property(node, "primary", NULL, 0);

	return node;
}

static uint32_t fsp_create_link(const struct spss_iopath *iopath, int index,
				int fsp_index)
{
	struct dt_node *node;
	const char *ststr;
	bool current = false;
	bool working = false;
	uint32_t chip_id;

	switch(be16_to_cpu(iopath->psi.link_status)) {
	case SPSS_IO_PATH_PSI_LINK_BAD_FRU:
		ststr = "Broken";
		break;
	case SPSS_IO_PATH_PSI_LINK_CURRENT:
		ststr = "Active";
		current = working = true;
		break;
	case SPSS_IO_PATH_PSI_LINK_BACKUP:
		ststr = "Backup";
		working = true;
		break;
	default:
		ststr = "Unknown";
	}
	prlog(PR_DEBUG, "FSP #%d: IO PATH %d is %s PSI Link, GXHB at %" PRIx64 "\n",
	      fsp_index, index, ststr, be64_to_cpu(iopath->psi.gxhb_base));

	chip_id = pcid_to_chip_id(be32_to_cpu(iopath->psi.proc_chip_id));
	node = dt_find_compatible_node_on_chip(dt_root, NULL, "ibm,psihb-x",
					       chip_id);
	if (!node) {
		prerror("FSP #%d: Can't find psihb node for link %d\n",
			fsp_index, index);
	} else {
		if (current)
			dt_add_property(node, "boot-link", NULL, 0);
		dt_add_property_strings(node, "status", working ? "ok" : "bad");
	}

	return chip_id;
}

static void fsp_create_links(const void *spss, int index,
			     struct dt_node *fsp_node)
{
	uint32_t *links = NULL;
	unsigned int i, lp, lcount = 0;
	int count;

	count = HDIF_get_iarray_size(spss, SPSS_IDATA_SP_IOPATH);
	if (count < 0) {
		prerror("FSP #%d: Can't find IO PATH array size !\n", index);
		return;
	}
	prlog(PR_DEBUG, "FSP #%d: Found %d IO PATH\n", index, count);

	/* Iterate all links */
	for (i = 0; i < count; i++) {
		const struct spss_iopath *iopath;
		unsigned int iopath_sz;
		uint32_t chip;

		iopath = HDIF_get_iarray_item(spss, SPSS_IDATA_SP_IOPATH,
					      i, &iopath_sz);
		if (!CHECK_SPPTR(iopath)) {
			prerror("FSP #%d: Can't find IO PATH %d\n", index, i);
			break;
		}
		if (be16_to_cpu(iopath->iopath_type) != SPSS_IOPATH_TYPE_PSI) {
			prerror("FSP #%d: Unsupported IO PATH %d type 0x%04x\n",
				index, i, iopath->iopath_type);
			continue;
		}

		chip = fsp_create_link(iopath, i, index);
		lp = lcount++;
		links = realloc(links, 4 * lcount);
		links[lp] = chip;
	}
	if (links)
		dt_add_property(fsp_node, "ibm,psi-links", links, lcount * 4);

	free(links);
}

static struct dt_node *add_lpc_io_node(struct dt_node *parent,
	const char *name, u32 offset, u32 size)
{
	struct dt_node *n;
	char buffer[32];

	/*
	 * LPC bus addresses have strange DT names, they have the
	 * Bus address space embedded into the unit address e.g.
	 * serial@i3f8 - refers to offset 0x3f8 in the IO space
	 */

	snprintf(buffer, sizeof(name), "%s@i%x", name, offset);
	n = dt_new(parent, buffer);
	assert(n);

	/* first address cell of 1 indicates the LPC IO space */
	dt_add_property_cells(n, "reg", 1, offset, size);

	return n;
}

static void add_uart(const struct spss_iopath *iopath, struct dt_node *lpc)
{
	struct dt_node *serial;
	u64 base;

	/* XXX: The spec says this is supposed to be a MMIO address.
	 *      However, in practice we get an LPC IO Space offset.
	 */
	base = be64_to_cpu(iopath->lpc.uart_base);

	serial = add_lpc_io_node(lpc, "serial", base,
		be32_to_cpu(iopath->lpc.uart_size));

	dt_add_property_string(serial, "compatible", "ns16550");

	dt_add_property_cells(serial, "current-speed",
		be32_to_cpu(iopath->lpc.uart_baud));
	dt_add_property_cells(serial, "clock-frequency",
		be32_to_cpu(iopath->lpc.uart_clk));
	dt_add_property_cells(serial, "interrupts",
		be32_to_cpu(iopath->lpc.uart_int_number));
	dt_add_property_string(serial, "device_type", "serial");


	prlog(PR_DEBUG, "LPC UART: base addr = %#" PRIx64" (%#" PRIx64 ") size = %#x clk = %u, baud = %u\n",
		be64_to_cpu(iopath->lpc.uart_base),
		base,
		be32_to_cpu(iopath->lpc.uart_size),
		be32_to_cpu(iopath->lpc.uart_clk),
		be32_to_cpu(iopath->lpc.uart_baud));
}

static void bmc_create_node(const struct HDIF_common_hdr *sp)
{
	u32 fw_bar, io_bar, mem_bar, internal_bar;
	const struct spss_iopath *iopath;
	struct dt_node *lpcm, *lpc, *n;
	u64 lpcm_base, lpcm_end;
	int chip_id;

	iopath = HDIF_get_iarray_item(sp, SPSS_IDATA_SP_IOPATH, 0, NULL);

	if (be16_to_cpu(iopath->iopath_type) != SPSS_IOPATH_TYPE_LPC) {
		prerror("BMC: Non-LPC IOPATH, this is probably broken\n");
		return;
	}

	/*
	 * For now we only instantiate the LPC node for the LPC that is used
	 * for Host <-> BMC comms. The secondary LPCs can be skipped.
	 */
	if (be16_to_cpu(iopath->lpc.link_status) != LPC_STATUS_ACTIVE)
		return;

#define GB (1024ul * 1024ul * 1024ul)
#define MMIO_LPC_BASE_P9 0x6030000000000ul
#define MMIO_STRIDE_P9     0x40000000000ul

	chip_id = be32_to_cpu(iopath->lpc.chip_id);

	lpcm_base = MMIO_LPC_BASE_P9 + MMIO_STRIDE_P9 * chip_id;
	lpcm = dt_new_addr(dt_root, "lpcm-opb", lpcm_base);
	assert(lpcm);

	dt_add_property_cells(lpcm, "#address-cells", 1);
	dt_add_property_cells(lpcm, "#size-cells", 1);
	dt_add_property_strings(lpcm, "compatible",
		"ibm,power9-lpcm-opb", "simple-bus");
	dt_add_property_u64s(lpcm, "reg", lpcm_base, 0x100000000ul);

	dt_add_property_cells(lpcm, "ibm,chip-id", chip_id);

	/* Setup the ranges for the MMIO LPC */
	lpcm_end = lpcm_base + 2 * GB;
	dt_add_property_cells(lpcm, "ranges",
		0x00000000, hi32(lpcm_base), lo32(lpcm_base), 2 * GB,
		0x80000000, hi32(lpcm_end),  lo32(lpcm_end),  2 * GB);

	/*
	 * Despite the name the "BAR" values provided through the HDAT are
	 * the base addresses themselves rather than the BARs
	 */
	fw_bar = be32_to_cpu(iopath->lpc.firmware_bar);
	mem_bar = be32_to_cpu(iopath->lpc.memory_bar);
	io_bar = be32_to_cpu(iopath->lpc.io_bar);
	internal_bar = be32_to_cpu(iopath->lpc.internal_bar);

	prlog(PR_DEBUG, "LPC: IOPATH chip id = %x\n", chip_id);
	prlog(PR_DEBUG, "LPC: FW BAR       = %#x\n", fw_bar);
	prlog(PR_DEBUG, "LPC: MEM BAR      = %#x\n", mem_bar);
	prlog(PR_DEBUG, "LPC: IO BAR       = %#x\n", io_bar);
	prlog(PR_DEBUG, "LPC: Internal BAR = %#x\n", internal_bar);

	/*
	 * The internal address space BAR actually points to the LPC master
	 * registers. So we "fix" it by masking off the low bits.
	 *
	 * XXX: we probably need separate base addresses for all these things
	 */
	internal_bar &= 0xf0000000;

	/* Add the various internal bus devices */
	n = dt_new_addr(lpcm, "opb-master", internal_bar + 0x10000);
	dt_add_property_string(n, "compatible", "ibm,power9-lpcm-opb-master");
	dt_add_property_cells(n, "reg", internal_bar + 0x10000, 0x60);

	n = dt_new_addr(lpcm, "opb-arbiter", internal_bar + 0x11000);
	dt_add_property_string(n, "compatible", "ibm,power9-lpcm-opb-arbiter");
	dt_add_property_cells(n, "reg", internal_bar + 0x11000, 0x8);

	n = dt_new_addr(lpcm, "lpc-controller", internal_bar + 0x12000);
	dt_add_property_string(n, "compatible", "ibm,power9-lpc-controller");
	dt_add_property_cells(n, "reg", internal_bar + 0x12000, 0x100);

	/*
	 * FIXME: lpc@0 might not be accurate, but i'm pretty sure
	 * lpc@f0000000 isn't right either.
	 */
	lpc = dt_new_addr(lpcm, "lpc", 0x0);
	dt_add_property_cells(lpc, "#address-cells", 2);
	dt_add_property_cells(lpc, "#size-cells", 1);
	dt_add_property_strings(lpc, "compatible",
				"ibm,power9-lpc", "ibm,power8-lpc");

	dt_add_property_cells(lpc, "ranges",
		0, 0, mem_bar, 0x10000000, /* MEM space */
		1, 0, io_bar,  0x00010000, /* IO space  */
		/* we don't expose the internal space */
		3, 0, fw_bar,  0x10000000  /* FW space  */
	);

	add_uart(iopath, lpc);

	/* BT device info isn't currently populated */
	prlog(PR_DEBUG, "LPC: BT [%#"PRIx64", %#x] sms_int: %u, bmc_int: %u\n",
		iopath->lpc.bt_base, iopath->lpc.bt_size,
		iopath->lpc.bt_sms_int_num, iopath->lpc.bt_bmc_response_int_num
	);
}

void fsp_parse(void)
{
	struct dt_node *fsp_root = NULL, *fsp_node;
	const void *sp;
	int index;

	/* Find SPSS tuple in SPIRA */
	sp = get_hdif(&spira.ntuples.sp_subsys, SPSS_HDIF_SIG);
	if (!sp) {
		prlog(PR_WARNING, "HDAT: No FSP/BMC found!\n");
		return;
	}

	for_each_ntuple_idx(&spira.ntuples.sp_subsys, sp, index, SPSS_HDIF_SIG) {
		switch (find_service_proc_type(sp, index)) {
		case SP_FSP:
			if (!fsp_root) {
				fsp_root = dt_new(dt_root, "fsps");
				assert(fsp_root);

				dt_add_property_cells(fsp_root,
					"#address-cells", 1);
				dt_add_property_cells(fsp_root,
					"#size-cells", 0);
			}

			fsp_node = fsp_create_node(sp, index, fsp_root);
			if (fsp_node)
				fsp_create_links(sp, index, fsp_node);

			break;

		case SP_BMC:
			bmc_create_node(sp);
			break;

		case SP_BAD:
			break;

		default:
			prerror("SP #%d: This service processor is not supported\n", index);
			break;
		}
	}
}
