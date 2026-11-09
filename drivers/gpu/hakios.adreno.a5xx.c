/*
 * sakura's GPU driver: the Adreno 506 (A5XX) of the msm8953.
 *
 * This is the A5XX counterpart of drivers/gpu/hakios.adreno.c (the Adreno
 * 642L of redwood/SC7280, which is compiled out under MSM8953). It speaks the
 * same kernel interface -- hakios_adreno_*() for the boot path, and the
 * hakios_gpu_*() set that iokit/Kernel/hakios.gpu.cpp (HakiosGPU) offers as
 * Linux's msm DRM requests, so hakios-metal needs no changes on this side.
 *
 * Facts from the sakura ROM FDT (devices/sakura/fw-rom/device-tree-rom.dtb):
 *   qcom,kgsl-3d0@1c00000   reg = <0x1c00000 0x40000>, qcom,chipid 0x5000600,
 *                            interrupts = <0x0 0x21 0x0> (SPI 33)
 *   qcom,kgsl-iommu@1c40000 (kgsl-smmu-v2), not a GICv3 SMMU
 *   clocks: core/iface/mem_iface/alt_mem_iface/rbbmtimer/alwayson (gcc)
 *   regulators: vddcx, vdd
 * Unlike the 642L there is **no GMU**: the 3D core is powered by clock + GDSC
 * + regulator (Linux drivers/gpu/drm/msm/adreno/a5xx_gpu.c, a5xx_gmu absent),
 * and the zap shader is loaded through TrustZone PAS (PAS_GPU) exactly as the
 * 642L path's step 12/19 do.
 *
 * State (Nov 10 2026): the interface and the machine facts are in place and
 * the file builds for the MSM8953 machine config; the step sequence is not yet
 * written (the phone is still bootloader-locked, so none of it can be seen).
 * The steps are listed in the steps[] table below with their Linux references.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

/* Pull the machine config explicitly (as osfmk/arm64/proc_reg.h does): on the
 * MSM8953 build (MACHINE_FLAGS defines ARM64_BOARD_CONFIG_MSM8953) it selects
 * pexpert/arm64/MSM8953.h, which defines GENERIC_GIC_V2_IRQ. Relying on the
 * transitive chain differs per TU. */
#include <pexpert/arm64/board_config.h>

/* The MSM8953 machine config (pexpert/arm64/MSM8953.h) defines this; it is the
 * macro a bsd translation unit reliably sees once board_config.h is included. */
#if defined(GENERIC_GIC_V2_IRQ)

#define ADRENO_BASE		0x01c00000UL	/* qcom,kgsl-3d0@1c00000 */
#define ADRENO_SIZE		0x00040000UL
#define QCOM_CHIPID		0x5000600UL	/* qcom,chipid: Adreno 506 */
#define GPU_IRQ_SPI		33		/* <0x0 0x21 0x0> */
#define KGSL_SMMU_BASE		0x01c40000UL	/* qcom,kgsl-iommu@1c40000 */

/*
 * A5XX register offsets, from Mesa's src/freedreno/registers/adreno/a5xx.xml
 * (generated a5xx.xml.h; see hakios-metal/tools/mesa/a5xx/README.md).
 */
#define REG_A5XX_RBBM_STATUS		0x04f5
#define REG_A5XX_CP_RB_BASE		0x0800
#define REG_A5XX_CP_RB_BASE_HI		0x0801
#define REG_A5XX_CP_RB_CNTL		0x0802
#define REG_A5XX_CP_RB_RPTR		0x0806
#define REG_A5XX_CP_RB_WPTR		0x0807
#define REG_A5XX_CP_INTERRUPT_STATUS	0x0b18
#define REG_A5XX_CP_HW_FAULT		0x0b1a
#define REG_A5XX_CP_PROTECT_STATUS	0x0b1c

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

/* The steps of waking the 506, mirroring the 642L driver's numbered steps. */
static const char *const a5xx_steps[] = {
	[0] = "identity (DT: qcom,chipid 0x5000600)",
	[1] = "clocks (gcc core/iface/mem_iface/rbbmtimer/alwayson)",
	[2] = "vddcx/vdd regulators + GDSC on",
	[3] = "kgsl SMMU (0x1c40000) + a context",
	[4] = "TrustZone PAS_GPU: load a506_zap (a5xx_gpu.c hw_init)",
	[5] = "command ring: CP_RB_BASE/CNTL/RPTR/WPTR",
	[6] = "GBIF-less A5XX: first CP packets (NOP, MEM_WRITE, ...)",
	[7] = "sysmem draw: the a5xx register sequence (fd5)",
	[8] = "keep for the system (adreno_keep) -> hakios_gpu_* live",
};
#define A5XX_STEPS ((unsigned)(sizeof(a5xx_steps) / sizeof(a5xx_steps[0])))

static unsigned a5xx_step_done;	/* highest step seen (0 = none) */
static bool a5xx_kept;		/* adreno_keep=1: leave the GPU on */

uint32_t gpu_log;		/* boot-arg gpu_log=1: a line per submission (the 642L driver's global) */

/*
 * Called while the system starts (hakios.input). The boot-arg contract is the
 * 642L driver's: gpu=1 starts and keeps the GPU for the system to draw with;
 * adreno=N runs step N alone. Until the steps below are implemented, nothing
 * of the GPU block is touched: identity is answered from the DT, and a run
 * only prints where the port stands.
 */
void
hakios_adreno_init(void)
{
	uint32_t only = 0;

	(void)PE_parse_boot_argn("gpu", &only, sizeof(only));
	if (!only) {
		return;
	}
	printf("adreno(a5xx): sakura Adreno 506 at 0x%x, chipid 0x%x, SPI %u\n",
	    (uint32_t)ADRENO_BASE, (uint32_t)QCOM_CHIPID, (uint32_t)GPU_IRQ_SPI);
	printf("adreno(a5xx): port status: steps 0..%u not yet implemented; "
	    "the bootloader is still locked, nothing on the GPU has been touched\n",
	    (uint32_t)(A5XX_STEPS - 1));
	a5xx_step_done = 0;
}

void
hakios_adreno_quiesce(void)
{
	/* no GPU work yet: nothing to stop */
}

void
hakios_adreno_stop(void)
{
	a5xx_kept = false;
}

void
hakios_adreno_minute(void)
{
	/* no GPU kept yet: say nothing */
}

/* ---- hakios_gpu_*: the interface HakiosGPU/hakios-metal talk to ---- */
/* All are "not ready" until the steps above exist. */

bool
hakios_gpu_ready(void)
{
	return false;
}

bool
hakios_gpu_parameter(uint32_t which, uint64_t *value)
{
	(void)which;
	(void)value;
	return false;
}

uint64_t
hakios_gpu_address(uint64_t size)
{
	(void)size;
	return 0;
}

void
hakios_gpu_address_free(uint64_t address, uint64_t size)
{
	(void)address;
	(void)size;
}

bool
hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	(void)address;
	(void)phys;
	(void)read_only;
	return false;
}

void
hakios_gpu_mapped(void)
{
}

void
hakios_gpu_unmap(uint64_t address, uint64_t size)
{
	(void)address;
	(void)size;
}

void
hakios_gpu_to_memory(void *start, uint64_t size)
{
	(void)start;
	(void)size;
}

uint32_t
hakios_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	(void)addresses;
	(void)words;
	(void)count;
	return 0;
}

bool
hakios_gpu_done(uint32_t fence)
{
	(void)fence;
	return false;
}

uint32_t
hakios_gpu_last(void)
{
	return 0;
}

void
hakios_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	*microseconds = 0;
	*submissions = 0;
	for (int i = 0; i < 7; i++) {
		by[i] = 0;
	}
}

void
hakios_gpu_report(const char *why)
{
	printf("adreno(a5xx): %s: the A5XX driver is not implemented yet\n", why);
}

void
hakios_gpu_stop(void)
{
	hakios_adreno_stop();
}

#endif /* defined(GENERIC_GIC_V2_IRQ) */
