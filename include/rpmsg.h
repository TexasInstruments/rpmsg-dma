#ifndef RPMSG_H
#define RPMSG_H

int send_msg(int fd, char *msg, int len);
int recv_msg(int fd, int len, char *reply_msg, int *reply_len);
int init_rpmsg(int rproc_id, int rmt_ep);
void cleanup_rpmsg(int fd);

#endif //RPMSG_H
