#ifndef HOST_INTERFACE_H
#define HOST_INTERFACE_H

#define LOG_QUEUE_SIZE          13000

void enqueue_log(const char *msg);
void *log_writer(void *arg);
void *cmd_listener(void *arg);
void init_host_interface();
int init_ethernet_async();

#endif //HOST_INTERFACE_H
