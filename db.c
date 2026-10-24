#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include "db.h"

static sqlite3 *db;

static int db_exec(const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "sqlite: %s\n", err ? err : sqlite3_errmsg(db));
		sqlite3_free(err);
		return 1;
	}
	return 0;
}

int db_open(const char *path)
{
	/* config 保存键值配置；book 保存每本书的总页数和当前页 */
	static const char schema[] =
		"CREATE TABLE IF NOT EXISTS config ("
		" key TEXT PRIMARY KEY,"
		" value TEXT NOT NULL);"
		"CREATE TABLE IF NOT EXISTS book ("
		" path TEXT PRIMARY KEY,"
		" pages INTEGER NOT NULL DEFAULT 0,"
		" page INTEGER NOT NULL DEFAULT 1);";

	if (db)
		return 0;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "cannot open <%s>: %s\n", path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return 1;
	}
	if (db_exec(schema)) {
		db_close();
		return 1;
	}
	return 0;
}

void db_close(void)
{
	if (!db)
		return;
	sqlite3_close(db);
	db = NULL;
}

int db_get_config(const char *key, char *value, size_t size)
{
	sqlite3_stmt *st = NULL;
	int found = 0;

	if (!db || !size || sqlite3_prepare_v2(db,
			"SELECT value FROM config WHERE key = ?",
			-1, &st, NULL) != SQLITE_OK)
		return 1;
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *v = sqlite3_column_text(st, 0);

		if (v) {
			snprintf(value, size, "%s", (const char *) v);
			found = 1;
		}
	}
	sqlite3_finalize(st);
	return found ? 0 : 1;
}

int db_set_config(const char *key, const char *value)
{
	sqlite3_stmt *st = NULL;
	int rc = 1;

	if (!db || sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)",
			-1, &st, NULL) != SQLITE_OK)
		return 1;
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_DONE)
		rc = 0;
	sqlite3_finalize(st);
	return rc;
}

int db_get_book(const char *path, int *pages, int *page)
{
	sqlite3_stmt *st = NULL;
	int found = 0;

	if (!db || sqlite3_prepare_v2(db,
			"SELECT pages, page FROM book WHERE path = ?",
			-1, &st, NULL) != SQLITE_OK)
		return 1;
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW) {
		if (pages)
			*pages = sqlite3_column_int(st, 0);
		if (page)
			*page = sqlite3_column_int(st, 1);
		found = 1;
	}
	sqlite3_finalize(st);
	return found ? 0 : 1;
}

int db_save_book(const char *path, int pages, int page)
{
	sqlite3_stmt *st = NULL;
	int rc = 1;

	if (!db || sqlite3_prepare_v2(db,
			"INSERT OR REPLACE INTO book (path, pages, page) VALUES (?, ?, ?)",
			-1, &st, NULL) != SQLITE_OK)
		return 1;
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 2, pages);
	sqlite3_bind_int(st, 3, page);
	if (sqlite3_step(st) == SQLITE_DONE)
		rc = 0;
	sqlite3_finalize(st);
	return rc;
}
