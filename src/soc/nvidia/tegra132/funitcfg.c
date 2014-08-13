/*
 * This file is part of the coreboot project.
 *
 * Copyright 2014 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <arch/io.h>
#include <soc/addressmap.h>
#include <soc/funitcfg.h>
#include <soc/clock.h>
#include <soc/padconfig.h>
#include <string.h>

struct clk_dev_control {
	uint32_t *clk_enb_set;
	uint32_t *rst_dev_clr;
};

struct funit_cfg_data {
	const char *name;
	uint32_t *clk_src_reg;
	const struct clk_dev_control * const dev_control;
	uint32_t clk_enb_val;
};

enum {
	CLK_L_SET = 0,
	CLK_H_SET = 1,
	CLK_U_SET = 2,
	CLK_V_SET = 3,
	CLK_W_SET = 4,
	CLK_X_SET = 5,
};

#define CLK_RST_REG(field_) \
	&(((struct clk_rst_ctlr *)TEGRA_CLK_RST_BASE)->field_)

#define CLK_SET_REGS(x)					\
	{						\
		CLK_RST_REG(clk_enb_##x##_set),		\
		CLK_RST_REG(rst_dev_##x##_clr),		\
	}

static const struct clk_dev_control clk_data_arr[] = {
	[CLK_L_SET] = CLK_SET_REGS(l),
	[CLK_H_SET] = CLK_SET_REGS(h),
	[CLK_U_SET] = CLK_SET_REGS(u),
	[CLK_V_SET] = CLK_SET_REGS(v),
	[CLK_W_SET] = CLK_SET_REGS(w),
	[CLK_X_SET] = CLK_SET_REGS(x),
};

#define FUNIT_DATA(funit_, loname_, clk_set_)				\
	[FUNIT_INDEX(funit_)] = {					\
		.name = STRINGIFY(loname_),				\
		.clk_src_reg = CLK_RST_REG(clk_src_##loname_),		\
		.dev_control = &clk_data_arr[CLK_##clk_set_##_SET],	\
		.clk_enb_val = CLK_##clk_set_##_##funit_,		\
	}

static const struct funit_cfg_data funit_data[] =  {
	FUNIT_DATA(SBC1, sbc1, H),
	FUNIT_DATA(SBC4, sbc4, U),
	FUNIT_DATA(I2C2, i2c2, H),
	FUNIT_DATA(I2C3, i2c3, U),
	FUNIT_DATA(I2C5, i2c5, H),
	FUNIT_DATA(SDMMC3, sdmmc3, U),
	FUNIT_DATA(SDMMC4, sdmmc4, L),
};

static inline uint32_t get_clk_src_freq(uint32_t clk_src)
{
	uint32_t freq = 0;

	switch(clk_src) {
	case CLK_M:
		freq = TEGRA_CLK_M_KHZ;
		break;
	case PLLP:
		freq = TEGRA_PLLP_KHZ;
		break;
	default:
		printk(BIOS_SPEW, "%s ERROR: Unknown clk_src %d\n",
		       __func__,clk_src);
	}

	return freq;
}

void soc_configure_funits(const struct funit_cfg * const entries, size_t num)
{
	size_t i;
	const char *funit_i2c = "i2c";
	uint32_t clk_div;
	uint32_t clk_div_mask;

	for (i = 0; i < num; i++) {
		const struct funit_cfg * const entry = &entries[i];
		const struct funit_cfg_data *funit;
		const struct clk_dev_control *dev_control;
		uint32_t clk_src_freq;

		if (entry->funit_index >= FUNIT_INDEX_MAX) {
			printk(BIOS_ERR, "Error: Index out of bounds\n");
			continue;
		}

		funit = &funit_data[entry->funit_index];
		dev_control = funit->dev_control;

		clk_src_freq = get_clk_src_freq(entry->clk_src_id);

		if (strncmp(funit->name,funit_i2c,strlen(funit_i2c)) == 0) {
			/* I2C funit */
			clk_div = get_i2c_clk_div(clk_src_freq,
						  entry->clk_dev_freq_khz);
			clk_div_mask = CLK_DIV_MASK_I2C;
		} else {
			/* Non I2C */
			clk_div = get_clk_div(clk_src_freq,
						entry->clk_dev_freq_khz);
			clk_div_mask = CLK_DIV_MASK;
		}

		_clock_set_div(funit->clk_src_reg, funit->name, clk_div,
				clk_div_mask, entry->clk_src_id);

		clock_grp_enable_clear_reset(funit->clk_enb_val,
						dev_control->clk_enb_set,
						dev_control->rst_dev_clr);

		soc_configure_pads(entry->pad_cfg,entry->pad_cfg_size);
	}
}
