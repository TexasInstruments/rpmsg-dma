#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
//#include <linux/dma-buf.h>
#include <errno.h>
#include "dmabuf.h"
#include "remoteproc_cdev.h"

// ========================= DMA Heap Utilities ================================

/* Open /dev/dma-heap/<heap_name>
 * This can be cma-reserved or another carveout heap we have in the dts.
 */
int dmaheap_open(char *heap_name)
{
	char name[100];
	int ret;

	snprintf(name, sizeof(name), "/dev/dma_heap/%s", heap_name);
	ret = open(name, O_RDWR);
	if (ret < 0)
		printf("Failed to open %s: -%d\n", name, errno);

	return ret;
}

/* Allocate dma-buf and return its file descriptor */
int dmaheap_alloc(int fd, size_t len)
{
	struct dma_heap_allocation_data data = {
		.fd_flags = O_CLOEXEC | O_RDWR,
		.len = len,
	};
	int ret;

	ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &data);
	if (ret < 0) {
		printf("ioctl DMA_HEAP_IOCTL_ALLOC failed with size %ld: -%d\n",
		       len, errno);
		return ret;
	}

	return data.fd;
}

/* Get the dma-heap buffer physical address from remoteproc cdev */
int dmabuf_get_phys(int rproc_fd, int dma_buf_fd, u_int64_t *phys_addr)
{
	struct rproc_dma_buf_attach_data data = {
		.fd = dma_buf_fd,
	};
	int ret;

	ret = ioctl(rproc_fd, RPROC_IOC_DMA_BUF_ATTACH, &data);
	if (ret < 0) {
		printf("ioctl DMA_BUF_PHYS_IOC_CONVERT failed: -%d\n",
		       errno);
		return ret;
	}
	*phys_addr = data.da;

	return 0;
}

int dmabuf_heap_init(char *heap_name, uint32_t buffer_size, char *rproc_dev, struct dma_buf_params *params)
{
	int ret = -1;

	/* Open the requested dma-heap device */
	params->dma_heap_fd = dmaheap_open(heap_name);
	if (params->dma_heap_fd < 0) {
		return params->dma_heap_fd;
	}

	params->dma_buf_fd = dmaheap_alloc(params->dma_heap_fd, buffer_size);
	if (params->dma_buf_fd < 0)
		return -1;

	params->rproc_fd = open(rproc_dev, O_RDONLY);
	if (params->rproc_fd < 0) {
		close(params->dma_buf_fd);
		printf("Failed to open %s: -%d\n", rproc_dev, errno);
		return -1;
	}

	ret = dmabuf_get_phys(params->rproc_fd, params->dma_buf_fd, &params->phys_addr);
	if (ret < 0) {
		close(params->dma_buf_fd);
		close(params->rproc_fd);
		return ret;
	}
	if (params->phys_addr > ~0UL) {
		printf("Can't pass buffer @%llx (64-bit adress) to the remote endpoint.\n",
		       (unsigned long long) params->phys_addr);
		close(params->dma_buf_fd);
		close(params->rproc_fd);
		return -1;
	}

	params->kern_addr = mmap(NULL, buffer_size, PROT_WRITE | PROT_READ, MAP_SHARED,
	                         params->dma_buf_fd, 0);

	if (params->kern_addr == MAP_FAILED) {
		printf("Mapping dma-buf failed: -%d\n", errno);
		close(params->dma_buf_fd);
		close(params->rproc_fd);
		return -1;
	}
	params->size = buffer_size;
	return 0;
}

void dmabuf_heap_destroy(struct dma_buf_params *params)
{
	munmap(params->kern_addr, params->size);
	close(params->dma_buf_fd);
	close(params->rproc_fd);
}

/* Indicate start/end of a map access session.*/
int dmabuf_sync(int fd, int start_stop)
{
	struct dma_buf_sync sync = {
		.flags = start_stop | DMA_BUF_SYNC_RW,
	};

	return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
