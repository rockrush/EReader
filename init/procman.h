#ifndef INIT_PROCMAN_H
#define INIT_PROCMAN_H

#include <sys/types.h>
#include <time.h>

#define MAX_SERVICES 64
#define MAX_ARGS 32

typedef enum {
	RESTART_NEVER = 0,
	RESTART_ALWAYS,
	RESTART_ON_FAILURE
} restart_policy_t;

typedef void (*service_func_t)(void);

typedef struct {
	char name[64];
	char *argv[MAX_ARGS + 1];
	int argc;
	service_func_t func;
	int builtin;
	pid_t pid;
	int running;
	restart_policy_t policy;
	int max_restarts;
	int restart_count;
	time_t last_start;
	int backoff_sec;
	int critical;   /* if 1, reboot on permanent failure */
	int enabled;
	int exit_status;
	int normal_exit; /* 1 if exited 0 */
} service_t;

void procman_init(void);
int  service_register(const char *name, char **argv, int argc,
					  restart_policy_t policy, int max_restarts, int critical);
int  service_register_builtin(const char *name, service_func_t func,
							 restart_policy_t policy, int max_restarts, int critical);
int  service_start(service_t *s);
void service_stop(service_t *s);
void reap_children(void);
void restart_failed_services(void);
service_t *service_find(const char *name);
service_t *service_list(int *count);
int  service_count(void);

#endif