#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include "host_interface.h"
#include "config.h"

int uart_fd;
int server_log_fd, client_log_fd;
int server_cmd_fd, client_cmd_fd;
pthread_t log_thread, cmd_thread, net_thread_log, net_thread_cmd;

#define LOG_PORT    8888
#define CMD_PORT    8889

struct sockaddr_in serv_addr, client_addr;
socklen_t addr_len = sizeof(client_addr);

typedef struct {
	char log[3000];
} LogEntry;
LogEntry log_queue[LOG_QUEUE_SIZE];
int log_head = 0, log_tail = 0;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t log_cond = PTHREAD_COND_INITIALIZER;

pthread_t net_thread;
volatile bool client_connected = false;

extern void set_zero_fft_index(int32_t value);

void* wait_for_log_client(void* arg)
{
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);
	listen(server_log_fd, 1);
	printf("Waiting for GUI log connection on port %d...\n", LOG_PORT);
	client_log_fd = accept(server_log_fd, (struct sockaddr *)&client, &client_len);
	printf("Log channel connected: %s\n", inet_ntoa(client.sin_addr));
	fflush(stdout);
	return NULL;
}

void* wait_for_cmd_client(void* arg)
{
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);
	listen(server_cmd_fd, 1);
	printf("Waiting for GUI cmd connection on port %d...\n", CMD_PORT);
	client_cmd_fd = accept(server_cmd_fd, (struct sockaddr *)&client, &client_len);
	printf("Cmd channel connected: %s\n", inet_ntoa(client.sin_addr));
	printf("Cmd channel accepted on fd %d\n", client_cmd_fd);
	fflush(stdout);
	return NULL;
}

int init_log_interface()
{
	struct sockaddr_in server;
	int opt = 1;
	server_log_fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(server_log_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	server.sin_family = AF_INET;
	server.sin_port = htons(LOG_PORT);
	server.sin_addr.s_addr = INADDR_ANY;
	bind(server_log_fd, (struct sockaddr *)&server, sizeof(server));
	pthread_create(&net_thread_log, NULL, wait_for_log_client, NULL);
	return 0;
}

int init_cmd_interface()
{
	struct sockaddr_in server;
	int opt = 1;
	server_cmd_fd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(server_cmd_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	server.sin_family = AF_INET;
	server.sin_port = htons(CMD_PORT);
	server.sin_addr.s_addr = INADDR_ANY;
	bind(server_cmd_fd, (struct sockaddr *)&server, sizeof(server));
	pthread_create(&net_thread_cmd, NULL, wait_for_cmd_client, NULL);
	return 0;
}

//====================== Logging Thread =========================

void enqueue_log(const char *msg)
{
	if(client_log_fd >= 0) {
		pthread_mutex_lock(&log_mutex);
		snprintf(log_queue[log_tail].log, sizeof(log_queue[log_tail].log), "%s", msg);
		log_tail = (log_tail + 1) % LOG_QUEUE_SIZE;
		pthread_cond_signal(&log_cond);
		pthread_mutex_unlock(&log_mutex);
	}
}

void *log_writer(void *arg)
{
	if(client_log_fd >= 0) {
		while (1) {
			pthread_mutex_lock(&log_mutex);
			while (log_head == log_tail)
				pthread_cond_wait(&log_cond, &log_mutex);
			char msg[256];
			strncpy(msg, log_queue[log_head].log, sizeof(msg));
			log_head = (log_head + 1) % LOG_QUEUE_SIZE;
			pthread_mutex_unlock(&log_mutex);
			if (client_log_fd >= 0) {
				dprintf(client_log_fd, "%s\n", msg);
			}
			fflush(stdout);
		}
	}
	return NULL;
}

void *cmd_listener(void *arg)
{
	char buf[256];
	int n;
	fd_set readset;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

	while (1) {
		FD_ZERO(&readset);
		FD_SET(client_cmd_fd, &readset);

		int ret = select(client_cmd_fd+1, &readset, NULL, NULL, &tv);
		if (ret < 0) {
			perror("select");
			continue;
		}
		if (ret == 0) {
			continue;
		}
		if (FD_ISSET(client_cmd_fd, &readset)) {
			n = read(client_cmd_fd, buf, sizeof(buf)-1);
			if (n <= 0) {
				printf("cmd_listener: read returned %d\n", n);
				close(client_cmd_fd);
				break;
			}
			buf[n] = '\0';

			static char linebuf[512];
			static int lineofs = 0;
			for (int i = 0; i < n; i++) {
				char c = buf[i];
				if (c == '\r') continue;
				if (c == '\n' || lineofs >= (int)sizeof(linebuf)-1) {
					linebuf[lineofs] = '\0';
					if (lineofs > 0) {
						if (strncmp(linebuf, "SET FFT INDEX ", 14)==0) {
							int32_t v;
							sscanf(linebuf+14, "%d",&v);
							set_zero_fft_index(v);
						}
					}
					lineofs = 0;
				} else {
					linebuf[lineofs++] = c;
				}
			}
		}
	}
	return NULL;
}

void init_host_interface()
{

	if(app_config.is_host_eth_iface) {
		init_log_interface();
		init_cmd_interface();
	} else {
		uart_fd = open(app_config.uart_device, O_RDWR | O_NOCTTY);
		if (uart_fd < 0) perror("UART open failed"), exit(1);

		struct termios tty;
		tcgetattr(uart_fd, &tty);
		cfsetospeed(&tty, B115200);
		cfsetispeed(&tty, B115200);
		tty.c_cflag |= (CLOCAL | CREAD);
		tty.c_cflag &= ~CSIZE;
		tty.c_cflag |= CS8;
		tty.c_cflag &= ~PARENB & ~CSTOPB & ~CRTSCTS;
		tty.c_lflag = tty.c_oflag = 0;
		tty.c_cc[VMIN] = 1;
		tcsetattr(uart_fd, TCSANOW, &tty);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	pthread_t log_thread;
	pthread_create(&log_thread, NULL, log_writer, NULL);
	pthread_create(&cmd_thread, NULL, cmd_listener, NULL);
}

