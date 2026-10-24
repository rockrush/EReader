#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>

#include "draw.h"
#include "ereader.h"
#include "db.h"

#define DEFAULT_PATH	"data"
#define DB_FILE		"ereader.db"	// 配置数据库文件名

struct ereader_cfg cfg;

int bpp;	// bytes per pixel
int cur_page;	// 当前页码

void graph_mode(bool state);
int font_load(void);
void font_close(void);
void *draw_ui(void *arg);
int show_text(const char *path);
int show_doc(char *path);
int show_filemgr(char *path);

/* 粗略判断文本文件：前 4KB 无 NUL 字节，且至少九成是可打印字符 */
int is_text(const char *path)
{
	unsigned char buf[4096];
	int n, i, good = 0;
	FILE *fp = fopen(path, "rb");

	if (!fp)
		return 0;
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (n <= 0)
		return 1;		/* 空文件按文本处理 */
	for (i = 0; i < n; i++) {
		unsigned char c = buf[i];

		if (c == 0)
			return 0;
		if (c == '\n' || c == '\r' || c == '\t' ||
				(c >= 0x20 && c != 0x7f))
			good++;
	}
	return good * 10 >= n * 9;
}

int is_pdf(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && dot != name && !strcasecmp(dot, ".pdf");
}

static void ebook(void) {
	pthread_t th_ui;
	struct stat st;
	char tmp_path[PATH_MAX];
	if (!realpath(cfg.path, tmp_path))
		strncpy(tmp_path, "/", 2);

	if (font_load()) {
		perror("loading fonts");
		return;
	}
	if (cfg.ui_w)
		// TODO: pthread_join
		if (pthread_create(&th_ui, NULL, draw_ui, NULL) != 0) {
			perror("creating UI thread");
			return;
		}

	while (true) {
		if (!tmp_path[0])
			strncpy(tmp_path, "/", 2);
		char *base = strrchr(tmp_path, '/');	// 定位文件名

		if (stat(tmp_path, &st) != 0)	// 读不了：不存在或没权限
			goto _parent;
		if (S_ISDIR(st.st_mode)) {
			show_filemgr(tmp_path);
			goto _parent;
		} else if (!S_ISREG(st.st_mode))
			goto _parent;

		if (is_pdf(base)) {
			show_doc(tmp_path);
			goto _parent;
		}

		// TODO: 其他文件格式

		if (is_text(tmp_path))
			show_text(tmp_path);
_parent:
		base[0] = 0;
	}
	font_close();
}

static void sigcont(int sig) {
	graph_mode(true);
}
static void sigint(int sig) {
	graph_mode(false);
	font_close();
	fb_free();
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	if (db_open(DB_FILE)) {
		fprintf(stderr, "cannot open <%s>\n", DB_FILE);
		return 1;
	}
	if (db_get_config("path", cfg.path, sizeof(cfg.path))) {
		strncpy(cfg.path, DEFAULT_PATH, sizeof(cfg.path) - 1);
		cfg.path[sizeof(cfg.path) - 1] = 0;
		if (db_set_config("path", cfg.path))
			fprintf(stderr, "cannot save path to <%s>\n", DB_FILE);
	}

	if (fb_init("/dev/fb0")) {
		db_close();
		return 1;
	}
	cfg.running = true;

	graph_mode(true);
	signal(SIGCONT, sigcont);
	signal(SIGINT, sigint);
	signal(SIGTERM, sigint);
	ebook();
	graph_mode(false);
	fb_free();
	db_close();
	return 0;
}
