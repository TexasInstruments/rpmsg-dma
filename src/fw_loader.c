#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include"fw_loader.h" 

int read_state(char* remote_proc_state_path, char *buf, size_t maxlen) {
	int fd = open(remote_proc_state_path, O_RDONLY);
	if (fd < 0) {
		printf("open failed fd = %d\n", fd);
        	return -1;
    	}
    ssize_t len = read(fd, buf, maxlen - 1);
    if (len < 0) {
        printf("read failed len = %ld\n", len);
        close(fd);
        return -1;
    }
    buf[len] = '\0';
    // Strip newline
    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';
        close(fd);
    return 0;
}

int write_state_if_needed(char* remote_proc_state_path, const char *desired) {
    char current[32];
    if (read_state(remote_proc_state_path, current, sizeof(current)) < 0)
        return -1;

    if (strcmp(current, desired) == 0) {
        printf("C7x is already in '%s' state. Skipping.\n", desired);
        return 0;
    }

    int fd = open(remote_proc_state_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: ", remote_proc_state_path);
        perror("");
        return -1;
    }

    if (write(fd, desired, strlen(desired)) < 0) {
        fprintf(stderr, "Failed to write '%s' to %s: ", desired, remote_proc_state_path);
        perror("");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int update_symlink(const char *source_path, const char *target_path) {
    unlink(source_path); // Remove existing symlink
    if (symlink(target_path, source_path) != 0) {
        perror("symlink");
        return -1;
    }
    return 0;
}

int switch_firmware(char* new_fw, char* fw_link, char* remote_proc_state_path) {
    if (write_state_if_needed(remote_proc_state_path, "stop") < 0)
        return -1;

    if (update_symlink(fw_link, new_fw) < 0)
        return -1;

    if (write_state_if_needed(remote_proc_state_path, "start") < 0)
        return -1;

    return 0;
}
