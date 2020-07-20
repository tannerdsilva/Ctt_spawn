#define _GNU_SOURCE
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <termios.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>

typedef struct {
    int reading;
    int writing;
} tt_pipe;

typedef struct {
	pid_t launchResult;
	void* stackAllocation;
} tt_spawn_result;

tt_spawn_result tt_spawn_core(const char *path, const char **argv, const int argv_count, const char **env_keys, const char **env_values, int env_count, const char *wd, tt_pipe input, tt_pipe output, tt_pipe error, tt_pipe notify);