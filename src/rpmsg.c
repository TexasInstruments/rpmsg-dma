#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <rproc_id.h>
#include <ti_rpmsg_char.h>
#include "rpmsg.h"
#include "dmabuf.h"

// ======================== RPMSG Communication ===========================

int send_msg(int fd, char *msg, int len)
{
	int ret = 0;
	dmabuf_sync(fd, 1);
	ret = write(fd, msg, len);
	if (ret < 0) {
		perror("Can't write to rpmsg endpt device\n");
		return -1;
	}
	dmabuf_sync(fd, 0);
	return ret;
}

int recv_msg(int fd, int len, char *reply_msg, int *reply_len)
{
	int ret = 0;
	dmabuf_sync(fd, 1);
	ret = read(fd, reply_msg, len);
	if (ret < 0) {
		perror("Can't read from rpmsg endpt device\n");
		return -1;
	} else {
		*reply_len = ret;
	}
	dmabuf_sync(fd, 0);
	return 0;
}

/* Initializes the RPMSG communication. */
int init_rpmsg(int rproc_id, int rmt_ep)
{
	int ret;
	char eptdev_name[64] = { 0 };
	rpmsg_char_dev_t *rcdev;

	ret = rpmsg_char_init(NULL);
	if (ret) {
		printf("rpmsg_char_init failed, ret = %d\n", ret);
		return ret;
	}

	sprintf(eptdev_name, "rpmsg-char-%d-%d", rproc_id, getpid());
	printf("eptdev = %s\n", eptdev_name);
	rcdev = rpmsg_char_open(rproc_id, NULL, RPMSG_ADDR_ANY, rmt_ep, eptdev_name, 0);
	if (!rcdev) {
		perror("Can't create an endpoint device");
		return -EPERM;
	}
	printf("Created endpt device %s, fd = %d port = %d\n", eptdev_name, rcdev->fd, rcdev->endpt);
	return  rcdev->fd;
}

void cleanup_rpmsg(int fd)
{
	close(fd);
}

