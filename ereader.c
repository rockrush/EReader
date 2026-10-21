#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <libfyaml.h>

#include "draw.h"
#include "ereader.h"

struct ereader_cfg cfg = {
	.scr_w = 800,
	.scr_h = 600,
	.ui_h = 0,
	.rotate = 270,	// 固定旋转 270 度
};

int bpp;	// bytes per pixel
int cur_page;	// 当前页码

void graph_mode(bool state);
void fm_set_view(uint16_t w, uint16_t h);
void *draw_ui(void *arg);
int show_text(const char *path, uint16_t w, uint16_t h);
int show_pdf(char *path, uint16_t w, uint16_t h);
int show_filemgr(char *path, uint16_t w, uint16_t h);

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

	if (cfg.ui_h)
		if (pthread_create(&th_ui, NULL, draw_ui, &cfg.ui_h) != 0) {
			perror("creating UI thread");
			return;
		}

	fm_set_view(cfg.scr_w, cfg.scr_h - cfg.ui_h);
	while (true) {
		if (!tmp_path[0])
			strncpy(tmp_path, "/", 2);
		char *base = strrchr(tmp_path, '/');	// 定位文件名

		if (stat(tmp_path, &st) != 0)	// 读不了：不存在或没权限
			goto _parent;
		if (S_ISDIR(st.st_mode)) {
			show_filemgr(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
			goto _parent;
		} else if (!S_ISREG(st.st_mode))
			goto _parent;

		if (is_pdf(base)) {
			show_pdf(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
			goto _parent;
		}

		// TODO: 其他文件格式

		if (is_text(tmp_path))
			show_text(tmp_path, cfg.scr_w, cfg.scr_h - cfg.ui_h);
_parent:
		base[0] = 0;
	}
}

static void sigcont(int sig) {
	graph_mode(true);
}
static void sigint(int sig) {
	graph_mode(false);
	fb_free();
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	struct fy_document *fyd = fy_document_build_from_file(NULL, "./ereader.yml");
	if (!fyd) {
		fprintf(stderr, "cannot parse ereader.yml\n");
		return 1;
	}
	memset(cfg.path, 0, PATH_MAX);
	int count = fy_document_scanf(fyd, "/path %s /page %u", cfg.path, &cur_page);
	if (count != 2) {
		fy_document_destroy(fyd);
		return 1;
	}
	fy_document_destroy(fyd);

	if (fb_init("/dev/fb0"))
		return 1;
	cfg.scr_h = fb_rows();
	cfg.scr_w = fb_cols();
	cfg.ui_h = cfg.scr_h - cfg.scr_w * 210 / 297 - 1;
	if (cfg.ui_h < 32)
		cfg.ui_h = 0;
	bpp = FBM_BPP(fb_mode());

	graph_mode(true);
	signal(SIGCONT, sigcont);
	signal(SIGINT, sigint);
	signal(SIGTERM, sigint);
	ebook();
	graph_mode(false);

	fb_free();
	return 0;
}
