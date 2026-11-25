#ifndef INIT_LOG_H
#define INIT_LOG_H

#include <stdio.h>

void log_init(void);
void log_msg(const char *level, const char *fmt, ...);

#define LOG_INFO(fmt, ...) log_msg("INFO", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_msg("WARN", fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  log_msg("ERROR", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_msg("DEBUG", fmt, ##__VA_ARGS__)

#endif