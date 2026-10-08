/*
 * qdma_gpu_p2p.h -- GPUDirect-RDMA P2P extension for the Xilinx QDMA driver.
 *
 * Lets a QDMA MM queue DMA directly to/from NVIDIA GPU memory (no host-RAM
 * bounce): userspace cudaMalloc's a buffer, passes its device pointer here, and
 * the driver pins it via nvidia_p2p_get_pages(), maps it for the FPGA peer via
 * nvidia_p2p_dma_map_pages(), then submits a normal qdma_request whose SGL
 * dma_addr fields are the GPU's PCIe bus addresses (dma_mapped=1).
 *
 * Direction (QDMA is the bus master, in MM mode):
 *   to_device=1  -> H2C: QDMA reads GPU mem, writes card ep_addr  (GPU -> FPGA)
 *   to_device=0  -> C2H: QDMA reads card ep_addr, writes GPU mem  (FPGA -> GPU)
 */
#ifndef QDMA_GPU_P2P_H
#define QDMA_GPU_P2P_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct qdma_gpu_p2p_xfer {
	__u64 gpu_vaddr;   /* in:  GPU device pointer from cudaMalloc            */
	__u64 size;        /* in:  transfer length in bytes                      */
	__u64 ep_addr;     /* in:  card-side AXI address (DDR/BRAM)              */
	__u32 to_device;   /* in:  1 = H2C (GPU->FPGA), 0 = C2H (FPGA->GPU)      */
	__u32 flags;       /* in:  reserved, 0                                   */
	__u64 bytes_done;  /* out: bytes actually transferred                    */
};

/* 'q' collides with nothing else the cdev handles; nr kept high + distinct. */
#define QDMA_GPU_P2P_MAGIC  'q'
#define QDMA_GPU_P2P_XFER   _IOWR(QDMA_GPU_P2P_MAGIC, 0xE0, struct qdma_gpu_p2p_xfer)

#ifdef __KERNEL__
struct qdma_cdev;
long qdma_gpu_p2p_ioctl(struct qdma_cdev *xcdev, unsigned int cmd,
			unsigned long arg);
#endif

#endif /* QDMA_GPU_P2P_H */
