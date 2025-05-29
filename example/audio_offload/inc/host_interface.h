#ifndef HOST_INTERFACE_H
#define HOST_INTERFACE_H

#define LOG_QUEUE_SIZE          13000

#define DEBUG 0
#if DEBUG
#define DBG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

void enqueue_log(const char *msg);
void *log_writer(void *arg);
void *cmd_listener(void *arg);
void init_host_interface();
int init_ethernet_async();

#endif //HOST_INTERFACE_H
