/* SPDX-License-Identifier: GPL-2.0-only */

#include <arch/cpu.h>
#include <console/console.h>
#include <device/pci.h>
#include <cpu/x86/lapic.h>
#include <cpu/x86/mp.h>
#include <cpu/x86/msr.h>
#include <cpu/intel/smm_reloc.h>
#include <cpu/intel/turbo.h>
#include <intelblocks/cpulib.h>
#include <intelblocks/mp_init.h>
#include <romstage_handoff.h>
#include <soc/cpu.h>
#include <soc/msr.h>
#include <soc/pci_devs.h>
#include <soc/pm.h>
#include <soc/systemagent.h>
#include <cpu/x86/mtrr.h>
#include <cpu/intel/microcode.h>
#include <cpu/intel/common/common.h>

#include "chip.h"

static void soc_fsp_load(void)
{
	fsps_load(romstage_handoff_is_resume());
}

static void configure_isst(void)
{
	config_t *conf = config_of_soc();
	msr_t msr;

	if (conf->speed_shift_enable) {
		/*
		 * Kernel driver checks CPUID.06h:EAX[Bit 7] to determine if HWP
		 * is supported or not. coreboot needs to configure MSR 0x1AA
		 * which is then reflected in the CPUID register.
		 */
		msr = rdmsr(MSR_MISC_PWR_MGMT);
		msr.lo |= MISC_PWR_MGMT_ISST_EN; /* Enable Speed Shift */
		msr.lo |= MISC_PWR_MGMT_ISST_EN_INT; /* Enable Interrupt */
		msr.lo |= MISC_PWR_MGMT_ISST_EN_EPP; /* Enable EPP */
		wrmsr(MSR_MISC_PWR_MGMT, msr);
	} else {
		msr = rdmsr(MSR_MISC_PWR_MGMT);
		msr.lo &= ~MISC_PWR_MGMT_ISST_EN; /* Disable Speed Shift */
		msr.lo &= ~MISC_PWR_MGMT_ISST_EN_INT; /* Disable Interrupt */
		msr.lo &= ~MISC_PWR_MGMT_ISST_EN_EPP; /* Disable EPP */
		wrmsr(MSR_MISC_PWR_MGMT, msr);
	}
}

static void configure_misc(void)
{
	msr_t msr;

	config_t *conf = config_of_soc();

	msr = rdmsr(IA32_MISC_ENABLE);
	msr.lo |= (1 << 0);	/* Fast String enable */
	msr.lo |= (1 << 3);	/* TM1/TM2/EMTTM enable */
	wrmsr(IA32_MISC_ENABLE, msr);

	/* Set EIST status */
	cpu_set_eist(conf->eist_enable);

	/* Disable Thermal interrupts */
	msr.lo = 0;
	msr.hi = 0;
	wrmsr(IA32_THERM_INTERRUPT, msr);

	/* Enable package critical interrupt only */
	msr.lo = 1 << 4;
	msr.hi = 0;
	wrmsr(IA32_PACKAGE_THERM_INTERRUPT, msr);

	/* Enable PROCHOT */
	msr = rdmsr(MSR_POWER_CTL);
	msr.lo |= (1 << 0);	/* Enable Bi-directional PROCHOT as an input*/
	msr.lo |= (1 << 23);	/* Lock it */
	wrmsr(MSR_POWER_CTL, msr);
}

static void enable_lapic_tpr(void)
{
	msr_t msr;

	msr = rdmsr(MSR_PIC_MSG_CONTROL);
	msr.lo &= ~(1 << 10);	/* Enable APIC TPR updates */
	wrmsr(MSR_PIC_MSG_CONTROL, msr);
}

static void configure_dca_cap(void)
{
	uint32_t feature_flag;
	msr_t msr;

	/* Check feature flag in CPUID.(EAX=1):ECX[18]==1 */
	feature_flag = cpu_get_feature_flags_ecx();
	if (feature_flag & CPUID_DCA) {
		msr = rdmsr(IA32_PLATFORM_DCA_CAP);
		msr.lo |= 1;
		wrmsr(IA32_PLATFORM_DCA_CAP, msr);
	}
}

/*
 * The emulated ACPI timer allows replacing of the ACPI timer
 * (PM1_TMR) to have no impart on the system.
 */
static void enable_pm_timer_emulation(void)
{
	msr_t msr;

	if (!CONFIG_CPU_XTAL_HZ)
		return;

	/*
	 * The derived frequency is calculated as follows:
	 * (clock * msr[63:32]) >> 32 = target frequency.
	 * Back solve the multiplier so the 3.579545MHz ACPI timer frequency is used.
	 */
	msr.hi = (3579545ULL << 32) / CONFIG_CPU_XTAL_HZ;
	/* Set PM1 timer IO port and enable */
	msr.lo = (EMULATE_DELAY_VALUE << EMULATE_DELAY_OFFSET_VALUE) |
			EMULATE_PM_TMR_EN | (ACPI_BASE_ADDRESS + PM1_TMR);
	wrmsr(MSR_EMULATE_PM_TIMER, msr);
}


static void set_energy_perf_bias(u8 policy)
{
	msr_t msr;
	int ecx;

	/* Determine if energy efficient policy is supported. */
	ecx = cpuid_ecx(0x6);
	if (!(ecx & (1 << 3)))
		return;

	/* Energy Policy is bits 3:0 */
	msr = rdmsr(IA32_ENERGY_PERF_BIAS);
	msr.lo &= ~0xf;
	msr.lo |= policy & 0xf;
	wrmsr(IA32_ENERGY_PERF_BIAS, msr);
}

static void configure_c_states(void)
{
	msr_t msr;

	/* C-state Interrupt Response Latency Control 1 - package C6/C7 short */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_32768_NS | C_STATE_LATENCY_CONTROL_1_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_1, msr);

	/* C-state Interrupt Response Latency Control 2 - package C6/C7 long */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_32768_NS | C_STATE_LATENCY_CONTROL_2_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_2, msr);

	/* C-state Interrupt Response Latency Control 3 - package C8 */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_32768_NS |
		C_STATE_LATENCY_CONTROL_3_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_3, msr);

	/* C-state Interrupt Response Latency Control 4 - package C9 */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_32768_NS |
		C_STATE_LATENCY_CONTROL_4_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_4, msr);

	/* C-state Interrupt Response Latency Control 5 - package C10 */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_32768_NS |
		C_STATE_LATENCY_CONTROL_5_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_5, msr);
}

/* All CPUs including BSP will run the following function. */
void soc_core_init(struct device *cpu)
{
	/* Clear out pending MCEs */
	/* TODO(adurbin): This should only be done on a cold boot. Also, some
	 * of these banks are core vs package scope. For now every CPU clears
	 * every bank. */
	mca_configure();

	/* Enable the local CPU apics */
	enable_lapic_tpr();
	setup_lapic();

	/* Configure c-state interrupt response time */
	configure_c_states();

	/* Configure Enhanced SpeedStep and Thermal Sensors */
	configure_misc();

	/* Configure Intel Speed Shift */
	configure_isst();

	set_aesni_lock();

	/* Enable ACPI Timer Emulation via MSR 0x121 */
	enable_pm_timer_emulation();

	/* Enable Direct Cache Access */
	configure_dca_cap();

	/* Set energy policy */
	set_energy_perf_bias(ENERGY_POLICY_NORMAL);

	/* Enable Turbo */
	enable_turbo();

	/* Enable Vmx */
	set_vmx_and_lock();
}

static void per_cpu_smm_trigger(void)
{
	/* Relocate the SMM handler. */
	smm_relocate();
}

static void post_mp_init(void)
{
	/* Set Max Ratio */
	cpu_set_max_ratio();

	/*
	 * Now that all APs have been relocated as well as the BSP let SMIs
	 * start flowing.
	 */
	global_smi_enable_no_pwrbtn();

	/* Lock down the SMRAM space. */
	smm_lock();
}

static const struct mp_ops mp_ops = {
	/*
	 * Skip Pre MP init MTRR programming as MTRRs are mirrored from BSP,
	 * that are set prior to ramstage.
	 * Real MTRRs programming are being done after resource allocation.
	 */
	.pre_mp_init = soc_fsp_load,
	.get_cpu_count = get_cpu_count,
	.get_smm_info = smm_info,
	.get_microcode_info = get_microcode_info,
	.pre_mp_smm_init = smm_initialize,
	.per_cpu_smm_trigger = per_cpu_smm_trigger,
	.relocation_handler = smm_relocation_handler,
	.post_mp_init = post_mp_init,
};

void soc_init_cpus(struct bus *cpu_bus)
{
	if (mp_init_with_smm(cpu_bus, &mp_ops))
		printk(BIOS_ERR, "MP initialization failure.\n");

	/* Thermal throttle activation offset */
	configure_tcc_thermal_target();
}

int soc_skip_ucode_update(u32 current_patch_id, u32 new_patch_id)
{
	msr_t msr1;
	msr_t msr2;

	/*
	 * CFL and WHL CPU die are based on KBL CPU so we need to
	 * have this check, where CNL CPU die is not based on KBL CPU
	 * so skip this check for CNL.
	 */
	if (!CONFIG(SOC_INTEL_CANNONLAKE_ALTERNATE_HEADERS))
		return 0;

	/*
	 * If PRMRR/SGX is supported the FIT microcode load will set the msr
	 * 0x08b with the Patch revision id one less than the id in the
	 * microcode binary. The PRMRR support is indicated in the MSR
	 * MTRRCAP[12]. If SGX is not enabled, check and avoid reloading the
	 * same microcode during CPU initialization. If SGX is enabled, as
	 * part of SGX BIOS initialization steps, the same microcode needs to
	 * be reloaded after the core PRMRR MSRs are programmed.
	 */
	msr1 = rdmsr(MTRR_CAP_MSR);
	msr2 = rdmsr(MSR_PRMRR_PHYS_BASE);
	if (msr2.lo && (current_patch_id == new_patch_id - 1))
		return 0;

	return (msr1.lo & MTRR_CAP_PRMRR) &&
		(current_patch_id == new_patch_id - 1);
}
