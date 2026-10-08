/*
 * qdma_gpu_p2p.c -- GPUDirect-RDMA P2P for the Xilinx QDMA driver.
 *
 * Pins an NVIDIA GPU buffer with the nvidia_p2p API, maps it so the FPGA (the
 * QDMA PCIe function) can reach it as a peer, and submits a normal MM
 * qdma_request whose scatter-gather dma_addr entries are the GPU's PCIe bus
 * addresses.  Because the request is flagged dma_mapped=1 / no_memcpy=1, the
 * QDMA MM path uses sg->dma_addr verbatim and never touches sg->pg -- which is
 * mandatory here, GPU BAR memory has no struct page.
 *
 * Only MM mode is supported (the ST H2C path dereferences page_address(sg->pg)).
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "nv-p2p.h"
#include "libqdma/libqdma_export.h"
#include "qdma_mod.h"		/* struct xlnx_pci_dev (xpdev->dev_hndl) */
#include "cdev.h"
#include "qdma_gpu_p2p.h"

/* NVIDIA GPU page is 64 KB on these parts; get_pages requires 64 KB alignment */
#define QGP2P_PAGE_SHIFT	16
#define QGP2P_PAGE_SIZE		(1UL << QGP2P_PAGE_SHIFT)
#define QGP2P_PAGE_MASK		(~(QGP2P_PAGE_SIZE - 1))

struct qgp2p_session {
	struct nvidia_p2p_page_table *pt;
	u64 va_aligned;
};

/* bytes-per-page from the page_table enum (4KB=0, 64KB=1, 128KB=2) */
static unsigned long qgp2p_page_bytes(u32 page_size_enum)
{
	switch (page_size_enum) {
	case NVIDIA_P2P_PAGE_SIZE_4KB:   return 4096UL;
	case NVIDIA_P2P_PAGE_SIZE_64KB:  return 65536UL;
	case NVIDIA_P2P_PAGE_SIZE_128KB: return 131072UL;
	default:                         return QGP2P_PAGE_SIZE;
	}
}

/* Forced-revocation safety net; not hit on the normal put_pages() path. */
static void qgp2p_free_cb(void *data)
{
	struct qgp2p_session *s = data;

	if (s && s->pt) {
		nvidia_p2p_free_page_table(s->pt);
		s->pt = NULL;
	}
}

long qdma_gpu_p2p_ioctl(struct qdma_cdev *xcdev, unsigned int cmd,
			unsigned long arg)
{
	struct qdma_gpu_p2p_xfer x;
	struct qdma_dev_conf conf;
	struct qdma_request *req = NULL;
	struct qdma_sw_sg *sgl = NULL;
	struct nvidia_p2p_dma_mapping *map = NULL;
	struct qgp2p_session *sess = NULL;
	unsigned long dev_hndl, qhndl, pg_bytes;
	struct pci_dev *pdev;
	u64 va_a, off, len_a, remain;
	int rv, i, n;
	ssize_t done;

	if (cmd != QDMA_GPU_P2P_XFER)
		return -ENOTTY;
	if (copy_from_user(&x, (void __user *)arg, sizeof(x)))
		return -EFAULT;
	if (!x.size || !x.gpu_vaddr)
		return -EINVAL;

	dev_hndl = xcdev->xcb->xpdev->dev_hndl;
	/* validate direction via dir_init (bit per q_type) -- a queue handle of
	 * 0 is legitimate (e.g. qidx 0), so never test the handle for presence */
	if (!(xcdev->dir_init & (1 << (x.to_device ? Q_H2C : Q_C2H)))) {
		pr_err("qdma_p2p: %s has no %s queue\n", xcdev->name,
		       x.to_device ? "H2C" : "C2H");
		return -EINVAL;
	}
	qhndl = x.to_device ? xcdev->h2c_qhndl : xcdev->c2h_qhndl;

	memset(&conf, 0, sizeof(conf));
	rv = qdma_device_get_config(dev_hndl, &conf, NULL, 0);
	if (rv < 0)
		return rv;
	pdev = conf.pdev;
	if (!pdev)
		return -ENODEV;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return -ENOMEM;

	/* nvidia_p2p_get_pages wants 64 KB-aligned base + length */
	va_a  = x.gpu_vaddr & QGP2P_PAGE_MASK;
	off   = x.gpu_vaddr - va_a;
	len_a = (off + x.size + QGP2P_PAGE_SIZE - 1) & QGP2P_PAGE_MASK;
	sess->va_aligned = va_a;

	rv = nvidia_p2p_get_pages(0, 0, va_a, len_a, &sess->pt,
				  qgp2p_free_cb, sess);
	if (rv < 0) {
		pr_err("qdma_p2p: nvidia_p2p_get_pages(0x%llx,%llu) = %d\n",
		       va_a, len_a, rv);
		kfree(sess);
		return rv;
	}

	rv = nvidia_p2p_dma_map_pages(pdev, sess->pt, &map);
	if (rv < 0) {
		pr_err("qdma_p2p: nvidia_p2p_dma_map_pages = %d\n", rv);
		goto out_put;
	}

	pg_bytes = qgp2p_page_bytes(sess->pt->page_size);
	n = map->entries;

	sgl = kcalloc(n, sizeof(*sgl), GFP_KERNEL);
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!sgl || !req) {
		rv = -ENOMEM;
		goto out_unmap;
	}

	/* Build the SGL from GPU bus addresses. First page carries the
	 * sub-page offset; trim the last entry to the exact byte count. */
	remain = x.size;
	for (i = 0; i < n && remain; i++) {
		u64 seg_off = (i == 0) ? off : 0;
		u64 seg = pg_bytes - seg_off;

		if (seg > remain)
			seg = remain;
		sgl[i].pg = NULL;		/* GPU memory: no struct page */
		sgl[i].offset = 0;
		sgl[i].len = (unsigned int)seg;
		sgl[i].dma_addr = map->dma_addresses[i] + seg_off;
		sgl[i].next = NULL;
		if (i > 0)
			sgl[i - 1].next = &sgl[i];
		remain -= seg;
	}
	if (remain) {			/* mapping shorter than requested */
		rv = -EINVAL;
		goto out_free;
	}

	pr_info("qdma_p2p: %s %s ep 0x%llx <-> GPU bus 0x%llx (%d segs, %llu B) via %s\n",
		xcdev->name, x.to_device ? "H2C GPU->FPGA" : "C2H FPGA->GPU",
		x.ep_addr, (u64)map->dma_addresses[0], i, x.size,
		pci_name(pdev));

	req->sgl	= sgl;
	req->sgcnt	= i;
	req->count	= x.size;
	req->ep_addr	= x.ep_addr;
	req->write	= x.to_device ? 1 : 0;	/* H2C writes to card (GPU->FPGA) */
	req->dma_mapped	= 1;			/* dma_addr are GPU bus addrs */
	req->no_memcpy	= 1;
	req->timeout_ms	= 10000;

	done = qdma_request_submit(dev_hndl, qhndl, req);
	if (done < 0) {
		rv = (int)done;
		goto out_free;
	}
	x.bytes_done = done;
	rv = copy_to_user((void __user *)arg, &x, sizeof(x)) ? -EFAULT : 0;

out_free:
	kfree(req);
	kfree(sgl);
out_unmap:
	nvidia_p2p_dma_unmap_pages(pdev, sess->pt, map);
out_put:
	if (sess->pt)
		nvidia_p2p_put_pages(0, 0, sess->va_aligned, sess->pt);
	kfree(sess);
	return rv;
}
