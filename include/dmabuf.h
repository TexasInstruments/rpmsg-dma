#ifndef DMABUF_H
#define DMABUF_H

#include <linux/dma-buf.h>

struct dma_buf_params {
	int dma_heap_fd;
	int dma_buf_fd;
	int rproc_fd;
	uint32_t *kern_addr;
	uint64_t phys_addr;
	int size;
};

int dmabuf_heap_init(char *heap_name, uint32_t buffer_size, char *rproc_dev, struct dma_buf_params *params);
void dmabuf_heap_destroy(struct dma_buf_params *params);
int dmabuf_sync(int fd, int start_stop);

#endif // DMABUF_H
