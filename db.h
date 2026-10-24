#ifndef _DB_H
#define _DB_H
#include <stddef.h>

int db_open(const char *path);
void db_close(void);
int db_get_config(const char *key, char *value, size_t size);
int db_set_config(const char *key, const char *value);
int db_get_book(const char *path, int *pages, int *page);
int db_save_book(const char *path, int pages, int page);

#endif
