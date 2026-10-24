#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "ereader.h"
#include "font.h"
#include "draw.h"

#define FM_FONT_PX	12	/* 基准字号（像素），按屏幕缩放倍数放大 */

struct fm_entry {
	char name[NAME_MAX + 1];
	int isdir;
	int isreg;
};

struct fm_view {
	char path[PATH_MAX];
	struct fm_entry *ents;
	int n;
	int sel;
	int top;
};

extern int bpp;

int is_text(const char *path);
int is_pdf(const char *path);
int show_doc(char *path);
int show_text(char *path);

static void fm_trim_slash(char *path)
{
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		path[--len] = 0;
}

static int fm_cmp(const void *a, const void *b)
{
	const struct fm_entry *ea = a;
	const struct fm_entry *eb = b;
	int d;

	if (ea->isdir != eb->isdir)
		return ea->isdir ? -1 : 1;
	d = strcasecmp(ea->name, eb->name);
	return d ? d : strcmp(ea->name, eb->name);
}

static int fm_scan(const char *path, struct fm_entry **out, int *nout)
{
	struct fm_entry *ents = NULL;
	struct dirent *de;
	int n = 0, cap = 0;
	DIR *dir = opendir(path);

	if (!dir)
		return 1;
	while ((de = readdir(dir))) {
		char full[PATH_MAX];
		struct fm_entry *ent;
		struct stat st;
		size_t len;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(full, sizeof(full), "%s%s%s", path, strcmp(path, "/") ? "/" : "", de->d_name) >= (int) sizeof(full))
			continue;
		if (stat(full, &st))
			continue;
		if (n == cap) {
			int ncap = cap ? cap * 2 : 64;
			struct fm_entry *tmp = realloc(ents, (size_t) ncap * sizeof(*tmp));
			if (!tmp) {
				free(ents);
				closedir(dir);
				return 1;
			}
			ents = tmp;
			cap = ncap;
		}
		ent = &ents[n++];
		len = strlen(de->d_name);
		if (len > sizeof(ent->name) - 1)
			len = sizeof(ent->name) - 1;
		memcpy(ent->name, de->d_name, len);
		ent->name[len] = 0;
		ent->isdir = S_ISDIR(st.st_mode);
		ent->isreg = S_ISREG(st.st_mode);
	}
	closedir(dir);
	if (n > 1)
		qsort(ents, n, sizeof(*ents), fm_cmp);
	*out = ents;
	*nout = n;
	return 0;
}

static int fm_chdir(struct fm_view *v, const char *path, const char *select)
{
	struct fm_entry *ents;
	int n, i;

	if (fm_scan(path, &ents, &n))
		return 1;
	free(v->ents);
	v->ents = ents;
	v->n = n;
	strncpy(v->path, path, sizeof(v->path) - 1);
	v->path[sizeof(v->path) - 1] = 0;
	v->sel = 0;
	v->top = 0;
	if (select) {
		for (i = 0; i < n; i++)
			if (!strcmp(v->ents[i].name, select)) {
				v->sel = i;
				break;
			}
	}
	return 0;
}

static int fm_enter(struct fm_view *v)
{
	char next[PATH_MAX];

	if (v->n <= 0)
		return 1;
	int n = snprintf(next, sizeof(next), "%s%s%s", v->path, strcmp(v->path, "/") ? "/" : "", v->ents[v->sel].name);
	if (n < 0 || n >= (int) sizeof(next))
		return 1;
	if (v->ents[v->sel].isdir) {
		if (fm_chdir(v, next, NULL)) {
			fprintf(stderr, "cannot open <%s>\n", next);
			return 1;
		}
		return 0;
	}
	if (!v->ents[v->sel].isreg)
		return 1;	/* 设备、管道等不做处理 */
	if (is_pdf(v->ents[v->sel].name)) {
		show_doc(next);
		return 0;
	}
	if (!is_text(next))
		return 1;
	show_text(next);
	return 0;		/* 返回后重绘文件列表 */
}

static int fm_parent(struct fm_view *v)
{
	char parent[PATH_MAX];
	char child[NAME_MAX + 1];
	char *base;

	if (!strcmp(v->path, "/"))
		return 1;
	strncpy(parent, v->path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = 0;
	base = strrchr(parent, '/');
	child[0] = 0;
	if (base) {
		strncpy(child, base + 1, sizeof(child) - 1);
		child[sizeof(child) - 1] = 0;
		if (base == parent)
			parent[1] = 0;
		else
			*base = 0;
	}
	if (fm_chdir(v, parent, child)) {
		fprintf(stderr, "cannot open <%s>\n", parent);
		return 1;
	}
	return 0;
}

static void fm_draw(struct fm_view *v)
{
	int px = FM_FONT_PX * FBM_FONT_SCALE;
	int pad = 4 * FBM_FONT_SCALE;
	int line_h = font_line_height(px) + 2 * FBM_FONT_SCALE;
	int header_h = font_line_height(px) + 8 * FBM_FONT_SCALE;
	int y0 = header_h + pad / 2;
	int visible = (cfg.view_h - y0 - pad) / line_h;	// 可显示条目数

	if (visible < 1)
		visible = 1;
	if (v->sel < v->top)
		v->top = v->sel;
	if (v->sel >= v->top + visible)
		v->top = v->sel - visible + 1;
	if (v->top > v->n - visible)
		v->top = v->n - visible;
	if (v->top < 0)
		v->top = 0;

	fill_rect(0, 0, cfg.view_h, cfg.view_w, UI_BG, UI_BG, UI_BG);	// 填充背景
	text(pad, pad, v->path, px, UI_ICON, UI_ICON, UI_ICON);	// 地址栏
	fill_rect(header_h, 0, FBM_FONT_SCALE, cfg.view_w, UI_BORDER, UI_BORDER, UI_BORDER);	// 分隔线

	if (!v->n) {
		text(pad, header_h + FBM_FONT_SCALE, "(empty)", px, UI_BORDER, UI_BORDER, UI_BORDER);
		return;
	}
	for (int i = v->top; i < v->n && i < v->top + visible; i++) {
		const struct fm_entry *e = &v->ents[i];
		int y = y0 + (i - v->top) * line_h;
		unsigned fg = (i == v->sel) ? UI_BG : UI_ICON;
		unsigned bg = (i == v->sel) ? UI_ICON : UI_BG;

		if (i == v->sel)
			fill_rect(y, 0, line_h, cfg.view_w, bg, bg, bg);
		int x = text(pad, y + FBM_FONT_SCALE, e->name, px, fg, fg, fg);
		if (e->isdir)
			text(x, y + FBM_FONT_SCALE, "/", px, fg, fg, fg);
	}
}

int show_filemgr(char *path)
{
	struct fm_view v;
	struct stat st;
	int c, retval = EXIT_FAILURE;

	if (!path || !path[0])	// 参数错误
		return EXIT_FAILURE;
	if (stat(path, &st) != 0)	// 查不到状态
		return EXIT_FAILURE;
	if (!S_ISDIR(st.st_mode))	// 不是文件夹
		return EXIT_FAILURE;

	memset(&v, 0, sizeof(v));
	fm_trim_slash(path);
	if (fm_chdir(&v, path, NULL)) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto out;
	}
	fm_draw(&v);
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':
			if (!fm_parent(&v))
				fm_draw(&v);
			break;
		case 'e':
			if (!fm_enter(&v))
				fm_draw(&v);
			break;
		case 'j':
			if (++v.sel >= v.n)
				v.sel = 0;
			fm_draw(&v);
			break;
		case 'k':
			if (--v.sel < 0)
				v.sel = v.n - 1;
			fm_draw(&v);
			break;
		default:
			continue;
		}
	}
	retval = EXIT_SUCCESS;
out:
	free(v.ents);
	return retval;
}
