/*
 * sakura's storage: the eMMC on the msm8953's SDHCI host (sdhci@7824900).
 * The protocol is MMC commands over the SDHCI interface (not SCSI/UFS): the
 * shim's read-only probe (devices/sakura/kernel/emmc.c) proved the register
 * path and the card protocol; this driver is that engine in the kernel, with
 * a DMA bounce buffer of the kernel's own memory, and a block/character
 * device per GPT partition (like hakios.ufs.c's for UFS).
 *
 * The controller is left up by ABL (it booted from this eMMC); no clocks are
 * touched. Reads use the SDMA engine and a single contiguous kernel buffer
 * flushed (cleaned + invalidated) before each transfer, as the UFS driver
 * does -- after a flush the cache holds no stale lines, so the controller's
 * write to DRAM is what the next CPU read sees.
 *
 * Read only by construction: there is no MMC write path in this file, and the
 * devices' strategy/read routines refuse anything but reads.
 *
 * Boot-arg emmc=1 initiates. The block devices are /dev/emmc_NAME and the
 * character devices /dev/remmc_NAME, one per GPT partition.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define SDHCI_BASE	0x7824900UL	/* hc_mem, sakura live FDT: sdhci@7824900 */
#define SDHCI_MAP_SIZE	0x100

#define BLOCK		512u
#define MAX_PARTS	24

/* SDHCI registers (byte offsets; read/written as 32-bit words as the shim's probe did) */
#define SDHCI_DMA_ADDR	0x00
#define SDHCI_BLKSIZE	0x04
#define SDHCI_ARGUMENT	0x08
#define SDHCI_TRANS_MODE 0x0c
#define SDHCI_COMMAND	0x0e
#define SDHCI_RESP0	0x10
#define SDHCI_PRESENT_STATE 0x24
#define SDHCI_HOST_CTRL	0x28
#define SDHCI_CLOCK_CTRL 0x2c
#define SDHCI_IRQ_STATUS 0x30
#define SDHCI_IRQ_ENABLE 0x34
#define SDHCI_CAPABILITIES 0x40

#define SDHCI_STATE_CMD_INHIBIT	(1u << 0)
#define SDHCI_STATE_DATA_INHIBIT	(1u << 1)
#define SDHCI_STATE_DAT3		(1u << 24)

#define SDHCI_CMD_INDEX_SHIFT 24
#define SDHCI_CMD_RESP_LONG	(1u << 6)
#define SDHCI_CMD_DATA_PRESENT	(1u << 5)
#define SDHCI_CMD_RESP_48	(1u << 4)
#define SDHCI_CMD_CRC_EN	(1u << 3)
#define SDHCI_CMD_INDEX_EN	(1u << 2)

#define SDHCI_TRANS_READ	(1u << 4)
#define SDHCI_TRANS_MULTI	(1u << 5)
#define SDHCI_TRANS_BLKCNT_EN	(1u << 1)
#define SDHCI_TRANS_DMA_EN	(1u << 0)

#define SDHCI_IRQ_EN_ERRORS	(0xffffu << 16)
#define SDHCI_IRQ_EN_CMD	(1u << 0)
#define SDHCI_IRQ_EN_XFER	(1u << 1)

#define MMC_CMD2_ALL_SEND_CID	2
#define MMC_CMD9_SEND_CSD	9
#define MMC_CMD18_READ_MULTI	18

static uintptr_t hc;			/* mapped SDHCI block */
static uint8_t *dma_buf;		/* contiguous bounce buffer, kernel VA */
static uint64_t dma_pa;			/* its physical address */
static uint32_t dma_size;
static bool emmc_up;
static int emmc_bmajor = -1, emmc_cmajor = -1;
static LCK_GRP_DECLARE(emmc_grp, "hakios.emmc");
static LCK_MTX_DECLARE(emmc_lock, &emmc_grp);

struct emmc_part {
	char name[17];
	uint64_t first_lba, last_lba;
	uint32_t minor;
};
static struct emmc_part emmc_parts[MAX_PARTS];
static uint32_t emmc_nparts;

static inline uint32_t hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(hc + off);
}
static inline void hc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(hc + off) = v;
}

static uint64_t emmc_ticks(void)
{
	return mach_absolute_time();
}
static int emmc_timeout(uint64_t start, uint64_t ns)
{
	uint64_t elapsed;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
	return elapsed > ns;
}

static int wait_free(uint32_t bit)
{
	const uint64_t start = emmc_ticks();
	while (hc_rd(SDHCI_PRESENT_STATE) & bit) {
		if (emmc_timeout(start, 1000000000ULL))	/* 1 s */
			return 1;
	}
	return 0;
}

/*
 * One MMC command, with an optional read transfer through the SDMA engine.
 * buf/len are kernel VA and byte count; the DMA uses kvtophys(buf).
 */
static uint32_t mmc_cmd(uint32_t index, uint32_t arg, uint32_t flags, void *buf, uint32_t len)
{
	uint32_t cmd = (index << SDHCI_CMD_INDEX_SHIFT) | flags;
	const uint64_t start = emmc_ticks();
	uint32_t irq;

	if (wait_free(SDHCI_STATE_CMD_INHIBIT))
		return 1;
	hc_wr(SDHCI_ARGUMENT, arg);

	if (len) {
		uint32_t blk = (len <= BLOCK) ? len : BLOCK;
		uint32_t cnt = (len + BLOCK - 1) / BLOCK;
		hc_wr(SDHCI_BLKSIZE, (blk & 0xfff) << 12);
		hc_wr(SDHCI_BLKSIZE + 2, cnt & 0xffff);
		hc_wr(SDHCI_DMA_ADDR, (uint32_t)dma_pa);
		FlushPoC_DcacheRegion((vm_offset_t)buf, len);
		cmd |= SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_EN |
		    SDHCI_CMD_INDEX_EN;
		hc_wr(SDHCI_TRANS_MODE,
		    SDHCI_TRANS_READ | SDHCI_TRANS_BLKCNT_EN | SDHCI_TRANS_DMA_EN);
	}
	hc_wr(SDHCI_COMMAND, cmd);

	while (!(hc_rd(SDHCI_IRQ_STATUS) & (SDHCI_IRQ_EN_CMD | SDHCI_IRQ_EN_ERRORS))) {
		if (emmc_timeout(start, 1000000000ULL)) {
			hc_wr(SDHCI_IRQ_STATUS, 0xffffffffu);
			return 2;
		}
	}
	irq = hc_rd(SDHCI_IRQ_STATUS);
	hc_wr(SDHCI_IRQ_STATUS, 0xffffffffu);
	if (irq & SDHCI_IRQ_EN_ERRORS)
		return 3;

	if (len) {
		while (!(hc_rd(SDHCI_IRQ_STATUS) & (SDHCI_IRQ_EN_XFER | SDHCI_IRQ_EN_ERRORS))) {
			if (emmc_timeout(start, 1000000000ULL)) {
				hc_wr(SDHCI_IRQ_STATUS, 0xffffffffu);
				return 4;
			}
		}
		irq = hc_rd(SDHCI_IRQ_STATUS);
		hc_wr(SDHCI_IRQ_STATUS, 0xffffffffu);
		if (irq & SDHCI_IRQ_EN_ERRORS)
			return 5;
	}
	return 0;
}

static int read_blocks(uint32_t lba, uint32_t count)
{
	return mmc_cmd(MMC_CMD18_READ_MULTI, lba, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_EN |
	    SDHCI_CMD_INDEX_EN, dma_buf, count * BLOCK);
}

/* ---- GPT ---- */

static uint64_t le64_at(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static int read_partitions(void)
{
	if (read_blocks(0, 2))
		return 1;
	const uint8_t *gpt = dma_buf + BLOCK;
	if (gpt[0] != 'E' || gpt[1] != 'F' || gpt[2] != 'I' || gpt[3] != ' ')
		return 2;
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16;
	uint32_t esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	if (esz != 128 || nent == 0 || nent > 128)
		return 3;
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (blocks > dma_size / BLOCK)
		return 4;
	if (read_blocks(elba, blocks))
		return 5;
	for (uint32_t i = 0; i < nent && emmc_nparts < MAX_PARTS; i++) {
		const uint8_t *e = dma_buf + i * 128;
		uint64_t first = le64_at(e + 32);
		uint64_t last = le64_at(e + 40);
		if (!first)
			continue;
		uint32_t n = emmc_nparts;
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			emmc_parts[n].name[c] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '?' : 0);
		}
		emmc_parts[n].name[16] = 0;
		emmc_parts[n].first_lba = first;
		emmc_parts[n].last_lba = last;
		emmc_parts[n].minor = i + 1;
		emmc_nparts++;
	}
	return 0;
}

/* ---- the devices ---- */

static int emmc_open(dev_t dev, int flags, int fmt, struct proc *p);
static int emmc_close(dev_t dev, int flags, int fmt, struct proc *p);
static int emmc_read(dev_t dev, struct uio *uio, int ioflag);
static int emmc_write(dev_t dev, struct uio *uio, int ioflag);
static int emmc_ioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p);
static int emmc_psize(dev_t dev);
static void emmc_strategy(struct buf *bp);

static const struct bdevsw emmc_bdevsw = {
	.d_open     = emmc_open,
	.d_close    = emmc_close,
	.d_strategy = emmc_strategy,
	.d_ioctl    = emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = emmc_psize,
	.d_type     = D_DISK,
};
static const struct cdevsw emmc_cdevsw = {
	.d_open       = emmc_open,
	.d_close      = emmc_close,
	.d_read       = emmc_read,
	.d_write      = emmc_write,
	.d_ioctl      = emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static struct emmc_part *part_for(dev_t dev)
{
	int minor = minor(dev);
	for (uint32_t i = 0; i < emmc_nparts; i++) {
		if (emmc_parts[i].minor == (uint32_t)minor)
			return &emmc_parts[i];
	}
	return NULL;
}

static int emmc_open(dev_t dev, int flags __unused, int fmt __unused, struct proc *p __unused)
{
	return part_for(dev) ? 0 : ENXIO;
}
static int emmc_close(dev_t dev __unused, int flags __unused, int fmt __unused, struct proc *p __unused)
{
	return 0;
}
static int emmc_ioctl(dev_t dev __unused, u_long cmd __unused, caddr_t data __unused,
    int flag __unused, struct proc *p __unused)
{
	return ENOTTY;
}
static int emmc_psize(dev_t dev)
{
	struct emmc_part *part = part_for(dev);
	if (!part)
		return 0;
	return (int)((part->last_lba - part->first_lba + 1));
}

static bool part_name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 16; i++) {
		if (a[i] != b[i])
			return false;
		if (a[i] == 0)
			return true;
	}
	return true;
}

int
hakios_emmc_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	if (!emmc_up)
		return ENXIO;
	lck_mtx_lock(&emmc_lock);
	for (uint32_t i = 0; i < emmc_nparts; i++) {
		if (!part_name_eq(emmc_parts[i].name, name))
			continue;
		uint64_t start = emmc_parts[i].first_lba * BLOCK;
		uint64_t end = (emmc_parts[i].last_lba + 1) * BLOCK;
		uint64_t avail = end - start;
		if (offset >= avail) {
			lck_mtx_unlock(&emmc_lock);
			return EINVAL;
		}
		uint64_t want = len < avail - offset ? len : avail - offset;
		uint64_t done = 0;
		while (done < want) {
			uint64_t chunk = want - done;
			if (chunk > dma_size)
				chunk = dma_size;
			uint64_t lba = (start + offset + done) / BLOCK;
			uint32_t count = (uint32_t)((chunk + BLOCK - 1) / BLOCK);
			if (read_blocks((uint32_t)lba, count)) {
				lck_mtx_unlock(&emmc_lock);
				return EIO;
			}
			uint64_t take = chunk;
			if (take > (uint64_t)count * BLOCK)
				take = (uint64_t)count * BLOCK;
			memcpy(dst + done, dma_buf, (size_t)take);
			done += take;
		}
		if (bytes)
			*bytes = done;
		lck_mtx_unlock(&emmc_lock);
		return 0;
	}
	lck_mtx_unlock(&emmc_lock);
	return ENOENT;
}

static int emmc_read(dev_t dev, struct uio *uio, int ioflag __unused)
{
	struct emmc_part *part = part_for(dev);
	if (!part)
		return ENXIO;
	uint64_t offset = uio->uio_offset;
	int error = 0;
	while (uio_resid(uio) > 0) {
		uint64_t chunk = uio_resid(uio);
		if (chunk > dma_size)
			chunk = dma_size;
		uint64_t got;
		error = hakios_emmc_read_partition(part->name, offset, dma_buf, chunk, &got);
		if (error || got == 0)
			break;
		error = uiomove(dma_buf, (int)got, uio);
		if (error)
			break;
		offset += got;
	}
	return error;
}

static int emmc_write(dev_t dev __unused, struct uio *uio __unused, int ioflag __unused)
{
	return EROFS;	/* read only, by construction */
}

static void emmc_strategy(struct buf *bp)
{
	uint32_t part = (uint32_t)minor(buf_device(bp));
	uint64_t offset, len, size;
	int error;

	if (part >= MAX_PARTS || part >= emmc_nparts) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	size = (emmc_parts[part].last_lba - emmc_parts[part].first_lba + 1) * BLOCK;
	offset = (uint64_t)buf_blkno(bp) * BLOCK;
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > size) {
		len = size - offset;
	}
	error = hakios_emmc_read_partition(emmc_parts[part].name, offset,
	    (uint8_t *)buf_dataptr(bp), len, NULL);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

/* ---- init ---- */

static int emmc_identify(void)
{
	if (!(hc_rd(SDHCI_PRESENT_STATE) & SDHCI_STATE_DAT3))
		return 1;
	if (mmc_cmd(MMC_CMD2_ALL_SEND_CID, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_EN,
	    NULL, 0))
		return 2;
	if (mmc_cmd(MMC_CMD9_SEND_CSD, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_EN,
	    NULL, 0))
		return 3;
	return 0;
}

void
hakios_emmc_init(void)
{
	uint32_t only;
	vm_offset_t dma_va = 0;

	if (!PE_parse_boot_argn("emmc", &only, sizeof(only)) || !only)
		return;
	hc = ml_io_map(SDHCI_BASE, SDHCI_MAP_SIZE);
	if (!hc)
		return;
	dma_size = 64 * 1024;
	if (kmem_alloc_contig(kernel_map, &dma_va, round_page(dma_size), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
		return;
	dma_buf = (uint8_t *)dma_va;
	dma_pa = kvtophys((vm_offset_t)dma_buf);
	if (emmc_identify()) {
		printf("emmc: no card (state %#x)\n", hc_rd(SDHCI_PRESENT_STATE));
		return;
	}
	lck_mtx_lock(&emmc_lock);
	if (read_partitions()) {
		lck_mtx_unlock(&emmc_lock);
		printf("emmc: no GPT\n");
		return;
	}
	lck_mtx_unlock(&emmc_lock);
	emmc_up = true;
	emmc_bmajor = bdevsw_add(-1, &emmc_bdevsw);
	emmc_cmajor = emmc_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &emmc_cdevsw, emmc_bmajor);
	printf("emmc: %u partitions (sdhci@%x, chip 0x%x, bmajor %d)\n", emmc_nparts,
	    (uint32_t)SDHCI_BASE, hc_rd(SDHCI_RESP0), emmc_bmajor);
	for (uint32_t i = 0; i < emmc_nparts; i++) {
		char name[32];
		snprintf(name, sizeof(name), "emmc_%s", emmc_parts[i].name);
		if (emmc_cmajor >= 0) {
			(void)devfs_make_node(makedev(emmc_cmajor, emmc_parts[i].minor), DEVFS_CHAR,
			    UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		}
		snprintf(name, sizeof(name), "remmc_%s", emmc_parts[i].name);
		if (emmc_bmajor >= 0) {
			(void)devfs_make_node(makedev(emmc_bmajor, emmc_parts[i].minor), DEVFS_BLOCK,
			    UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		}
		printf("emmc:   %-16s lba %llu..%llu (%llu MiB)\n", emmc_parts[i].name,
		    emmc_parts[i].first_lba, emmc_parts[i].last_lba,
		    (emmc_parts[i].last_lba - emmc_parts[i].first_lba + 1) >> 9);
	}
}

void
hakios_emmc_quiesce(void)
{
	emmc_up = false;
}
