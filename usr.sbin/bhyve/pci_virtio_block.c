/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/linker_set.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "fbsdrun.h"
#include "pci_emul.h"
#include "virtio.h"

#define VTBLK_RINGSZ	64

#define VTBLK_CFGSZ	28

#define VTBLK_R_CFG		VTCFG_R_CFG0
#define VTBLK_R_CFG_END		VTBLK_R_CFG + VTBLK_CFGSZ -1
#define VTBLK_R_MAX		VTBLK_R_CFG_END

#define VTBLK_REGSZ		VTBLK_R_MAX+1

#define VTBLK_MAXSEGS	32

#define VTBLK_S_OK	0
#define VTBLK_S_IOERR	1

/*
 * Host capabilities
 */
#define VTBLK_S_HOSTCAPS      \
  ( 0x00000004 |	/* host maximum request segments */ \
    0x10000000 )	/* supports indirect descriptors */

struct vring_hqueue {
	/* Internal state */
	uint16_t	hq_size;
	uint16_t	hq_cur_aidx;		/* trails behind 'avail_idx' */

	 /* Host-context pointers to the queue */
	struct virtio_desc *hq_dtable;
	uint16_t	*hq_avail_flags;
	uint16_t	*hq_avail_idx;		/* monotonically increasing */
	uint16_t	*hq_avail_ring;

	uint16_t	*hq_used_flags;
	uint16_t	*hq_used_idx;		/* monotonically increasing */
	struct virtio_used *hq_used_ring;
};

/*
 * Config space
 */
struct vtblk_config {
	uint64_t	vbc_capacity;
	uint32_t	vbc_size_max;
	uint32_t	vbc_seg_max;
	uint16_t	vbc_geom_c;
	uint8_t		vbc_geom_h;
	uint8_t		vbc_geom_s;
	uint32_t	vbc_blk_size;
	uint32_t	vbc_sectors_max;
} __packed;
CTASSERT(sizeof(struct vtblk_config) == VTBLK_CFGSZ);

/*
 * Fixed-size block header
 */
struct virtio_blk_hdr {
#define VBH_OP_READ	0
#define VBH_OP_WRITE	1
	uint32_t       	vbh_type;
	uint32_t	vbh_ioprio;
	uint64_t	vbh_sector;
} __packed;

/*
 * Debug printf
 */
static int pci_vtblk_debug;
#define DPRINTF(params) if (pci_vtblk_debug) printf params
#define WPRINTF(params) printf params

/*
 * Per-device softc
 */
struct pci_vtblk_softc {
	struct pci_devinst *vbsc_pi;
	int		vbsc_fd;
	int		vbsc_status;
	int		vbsc_isr;
	int		vbsc_lastq;
	uint32_t	vbsc_features;
	uint64_t	vbsc_pfn;
	struct vring_hqueue vbsc_q;
	struct vtblk_config vbsc_cfg;	
};

/*
 * Return the number of available descriptors in the vring taking care
 * of the 16-bit index wraparound.
 */
static int
hq_num_avail(struct vring_hqueue *hq)
{
	int ndesc;

	if (*hq->hq_avail_idx >= hq->hq_cur_aidx)
		ndesc = *hq->hq_avail_idx - hq->hq_cur_aidx;
	else
		ndesc = UINT16_MAX - hq->hq_cur_aidx + *hq->hq_avail_idx + 1;

	assert(ndesc >= 0 && ndesc <= hq->hq_size);

	return (ndesc);
}

static void
pci_vtblk_update_status(struct pci_vtblk_softc *sc, uint32_t value)
{
	if (value == 0) {
		DPRINTF(("vtblk: device reset requested !\n"));
	}

	sc->vbsc_status = value;
}

static void
pci_vtblk_proc(struct pci_vtblk_softc *sc, struct vring_hqueue *hq)
{
	struct iovec iov[VTBLK_MAXSEGS];
	struct virtio_blk_hdr *vbh;
	struct virtio_desc *vd, *vid;
	struct virtio_used *vu;
	uint8_t *status;
	int i;
	int err;
	int iolen;
	int nsegs;
	int uidx, aidx, didx;
	int writeop;
	off_t offset;

	uidx = *hq->hq_used_idx;
	aidx = hq->hq_cur_aidx;
	didx = hq->hq_avail_ring[aidx % hq->hq_size];
	assert(didx >= 0 && didx < hq->hq_size);

	vd = &hq->hq_dtable[didx];

	/*
	 * Verify that the descriptor is indirect, and obtain
	 * the pointer to the indirect descriptor.
	 * There has to be space for at least 3 descriptors
	 * in the indirect descriptor array: the block header,
	 * 1 or more data descriptors, and a status byte.
	 */
	assert(vd->vd_flags & VRING_DESC_F_INDIRECT);

	nsegs = vd->vd_len / sizeof(struct virtio_desc);
	assert(nsegs >= 3);
	assert(nsegs < VTBLK_MAXSEGS + 2);

	vid = paddr_guest2host(vd->vd_addr);
	assert((vid->vd_flags & VRING_DESC_F_INDIRECT) == 0);

	/*
	 * The first descriptor will be the read-only fixed header
	 */
	vbh = paddr_guest2host(vid[0].vd_addr);
	assert(vid[0].vd_len == sizeof(struct virtio_blk_hdr));
	assert(vid[0].vd_flags & VRING_DESC_F_NEXT);
	assert((vid[0].vd_flags & VRING_DESC_F_WRITE) == 0);

	writeop = (vbh->vbh_type == VBH_OP_WRITE);

	offset = vbh->vbh_sector * DEV_BSIZE;

	/*
	 * Build up the iovec based on the guest's data descriptors
	 */
	for (i = 1, iolen = 0; i < nsegs - 1; i++) {
		iov[i-1].iov_base = paddr_guest2host(vid[i].vd_addr);
		iov[i-1].iov_len = vid[i].vd_len;
		iolen += vid[i].vd_len;

		assert(vid[i].vd_flags & VRING_DESC_F_NEXT);
		assert((vid[i].vd_flags & VRING_DESC_F_INDIRECT) == 0);

		/*
		 * - write op implies read-only descriptor,
		 * - read op implies write-only descriptor,
		 * therefore test the inverse of the descriptor bit
		 * to the op.
		 */
		assert(((vid[i].vd_flags & VRING_DESC_F_WRITE) == 0) ==
		       writeop);
	}

	/* Lastly, get the address of the status byte */
	status = paddr_guest2host(vid[nsegs - 1].vd_addr);
	assert(vid[nsegs - 1].vd_len == 1);
	assert((vid[nsegs - 1].vd_flags & VRING_DESC_F_NEXT) == 0);
	assert(vid[nsegs - 1].vd_flags & VRING_DESC_F_WRITE);

	DPRINTF(("virtio-block: %s op, %d bytes, %d segs, offset %ld\n\r", 
		 writeop ? "write" : "read", iolen, nsegs - 2, offset));

	if (writeop){
		err = pwritev(sc->vbsc_fd, iov, nsegs - 2, offset);
	} else {
		err = preadv(sc->vbsc_fd, iov, nsegs - 2, offset);
	}

	*status = err < 0 ? VTBLK_S_IOERR : VTBLK_S_OK;

	/*
	 * Return the single indirect descriptor back to the host
	 */
	vu = &hq->hq_used_ring[uidx % hq->hq_size];
	vu->vu_idx = didx;
	vu->vu_tlen = 1;
	hq->hq_cur_aidx++;
	*hq->hq_used_idx += 1;
}

static void
pci_vtblk_qnotify(struct pci_vtblk_softc *sc)
{
	struct vring_hqueue *hq = &sc->vbsc_q;
	int i;
	int ndescs;

	/*
	 * Calculate number of ring entries to process
	 */
	ndescs = hq_num_avail(hq);

	if (ndescs == 0)
		return;

	/*
	 * Run through all the entries, placing them into iovecs and
	 * sending when an end-of-packet is found
	 */
	for (i = 0; i < ndescs; i++)
		pci_vtblk_proc(sc, hq);

	/*
	 * Generate an interrupt if able
	 */
	if ((*hq->hq_avail_flags & VRING_AVAIL_F_NO_INTERRUPT) == 0 &&
		sc->vbsc_isr == 0) {
		sc->vbsc_isr = 1;
		pci_generate_msi(sc->vbsc_pi, 0);
	}
	
}

static void
pci_vtblk_ring_init(struct pci_vtblk_softc *sc, uint64_t pfn)
{
	struct vring_hqueue *hq;

	sc->vbsc_pfn = pfn << VRING_PFN;
	
	/*
	 * Set up host pointers to the various parts of the
	 * queue
	 */
	hq = &sc->vbsc_q;
	hq->hq_size = VTBLK_RINGSZ;

	hq->hq_dtable = paddr_guest2host(pfn << VRING_PFN);
	hq->hq_avail_flags =  (uint16_t *)(hq->hq_dtable + hq->hq_size);
	hq->hq_avail_idx = hq->hq_avail_flags + 1;
	hq->hq_avail_ring = hq->hq_avail_flags + 2;
	hq->hq_used_flags = (uint16_t *)roundup2((uintptr_t)hq->hq_avail_ring,
						 VRING_ALIGN);
	hq->hq_used_idx = hq->hq_used_flags + 1;
	hq->hq_used_ring = (struct virtio_used *)(hq->hq_used_flags + 2);

	/*
	 * Initialize queue indexes
	 */
	hq->hq_cur_aidx = 0;
}

static int
pci_vtblk_init(struct vmctx *ctx, struct pci_devinst *pi, char *opts)
{
	struct stat sbuf;
	struct pci_vtblk_softc *sc;
	int fd;

	if (opts == NULL) {
		printf("virtio-block: backing device required\n");
		return (1);
	}

	/*
	 * Access to guest memory is required. Fail if
	 * memory not mapped
	 */
	if (paddr_guest2host(0) == NULL)
		return (1);

	/*
	 * The supplied backing file has to exist
	 */
	fd = open(opts, O_RDWR);
	if (fd < 0) {
		perror("Could not open backing file");
		return (1);
	}

	if (fstat(fd, &sbuf) < 0) {
		perror("Could not stat backing file");
		close(fd);
		return (1);
	}
	
	sc = malloc(sizeof(struct pci_vtblk_softc));
	memset(sc, 0, sizeof(struct pci_vtblk_softc));

	pi->pi_arg = sc;
	sc->vbsc_pi = pi;
	sc->vbsc_fd = fd;

	/* setup virtio block config space */
	sc->vbsc_cfg.vbc_capacity = sbuf.st_size / DEV_BSIZE;
	sc->vbsc_cfg.vbc_seg_max = VTBLK_MAXSEGS;
	sc->vbsc_cfg.vbc_blk_size = DEV_BSIZE;
	sc->vbsc_cfg.vbc_size_max = 0;	/* not negotiated */
	sc->vbsc_cfg.vbc_geom_c = 0;	/* no geometry */
	sc->vbsc_cfg.vbc_geom_h = 0;
	sc->vbsc_cfg.vbc_geom_s = 0;
	sc->vbsc_cfg.vbc_sectors_max = 0;

	/* initialize config space */
	pci_set_cfgdata16(pi, PCIR_DEVICE, VIRTIO_DEV_BLOCK);
	pci_set_cfgdata16(pi, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_STORAGE);
	pci_set_cfgdata16(pi, PCIR_SUBDEV_0, VIRTIO_TYPE_BLOCK);
	pci_emul_alloc_bar(pi, 0, 0, PCIBAR_IO, VTBLK_REGSZ);
	pci_emul_add_msicap(pi, 1);

	return (0);
}

static void
pci_vtblk_write(struct pci_devinst *pi, int baridx, int offset, int size,
		uint32_t value)
{
	struct pci_vtblk_softc *sc = pi->pi_arg;
	
	if (offset + size > VTBLK_REGSZ) {
		DPRINTF(("vtblk_write: 2big, offset %d size %d\n",
			 offset, size));
		return;
	}

	switch (offset) {
	case VTCFG_R_GUESTCAP:
		assert(size == 4);
		sc->vbsc_features = value & VTBLK_S_HOSTCAPS;
		break;
	case VTCFG_R_PFN:
		assert(size == 4);
		pci_vtblk_ring_init(sc, value);
		break;
	case VTCFG_R_QSEL:
		assert(size == 2);
		sc->vbsc_lastq = value;
		break;
	case VTCFG_R_QNOTIFY:
		assert(size == 2);
		assert(value == 0);
		pci_vtblk_qnotify(sc);
		break;
	case VTCFG_R_STATUS:
		assert(size == 1);
		pci_vtblk_update_status(sc, value);
		break;
	case VTCFG_R_HOSTCAP:
	case VTCFG_R_QNUM:
	case VTCFG_R_ISR:
	case VTBLK_R_CFG ... VTBLK_R_CFG_END:
		DPRINTF(("vtblk: write to readonly reg %d\n\r", offset));
		break;
	default:
		DPRINTF(("vtblk: unknown i/o write offset %d\n\r", offset));
		value = 0;
		break;
	}
}

uint32_t
pci_vtblk_read(struct pci_devinst *pi, int baridx, int offset, int size)
{
	struct pci_vtblk_softc *sc = pi->pi_arg;
	void *ptr;
	uint32_t value;

	if (offset + size > VTBLK_REGSZ) {
		DPRINTF(("vtblk_read: 2big, offset %d size %d\n",
			 offset, size));
		return (0);
	}

	switch (offset) {
	case VTCFG_R_HOSTCAP:
		assert(size == 4);
		value = VTBLK_S_HOSTCAPS;
		break;
	case VTCFG_R_GUESTCAP:
		assert(size == 4);
		value = sc->vbsc_features; /* XXX never read ? */
		break;
	case VTCFG_R_PFN:
		assert(size == 4);
		value = sc->vbsc_pfn >> VRING_PFN;
		break;
	case VTCFG_R_QNUM:
		value = (sc->vbsc_lastq == 0) ? VTBLK_RINGSZ: 0;
		break;
	case VTCFG_R_QSEL:
		assert(size == 2);
		value = sc->vbsc_lastq; /* XXX never read ? */
		break;
	case VTCFG_R_QNOTIFY:
		assert(size == 2);
		value = 0; /* XXX never read ? */
		break;
	case VTCFG_R_STATUS:
		assert(size == 1);
		value = sc->vbsc_status;
		break;
	case VTCFG_R_ISR:
		assert(size == 1);
		value = sc->vbsc_isr;
		sc->vbsc_isr = 0;     /* a read clears this flag */
		break;
	case VTBLK_R_CFG ... VTBLK_R_CFG_END:
		assert(size + offset <= (VTBLK_R_CFG_END + 1));
		ptr = (uint8_t *)&sc->vbsc_cfg + offset - VTBLK_R_CFG;
		if (size == 1) {
			value = *(uint8_t *) ptr;
		} else if (size == 2) {
			value = *(uint16_t *) ptr;
		} else {
			value = *(uint32_t *) ptr;
		}
		break;
	default:
		DPRINTF(("vtblk: unknown i/o read offset %d\n\r", offset));
		value = 0;
		break;
	}

	return (value);
}

struct pci_devemu pci_de_vblk = {
	.pe_emu = "virtio-blk",
	.pe_init = pci_vtblk_init,
	.pe_iow = pci_vtblk_write,
	.pe_ior = pci_vtblk_read,
};
PCI_EMUL_SET(pci_de_vblk);
