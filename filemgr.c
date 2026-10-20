#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "draw.h"

#define FM_FONT_PX	12	/* 基准字号（像素），按屏幕缩放倍数放大 */
#define FM_FIRST	32
#define FM_LAST		126

struct fm_glyph {
	int w, h;		/* 位图尺寸 */
	int left, top;		/* 位图相对基线的偏移 */
	int advance;		/* 步进（像素） */
	unsigned char *bitmap;	/* 8 位灰度位图，行宽为 w */
	int loaded;
};

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

static int fm_font_users;	/* 字体由文件管理器和文本阅读器共用，最后一个使用者退出时才释放 */
static FT_Library fm_lib;
static FT_Face fm_face;
static struct fm_glyph fm_glyphs[FM_LAST - FM_FIRST + 1];
static int fm_px;	/* 当前字号（像素） */
static int fm_ascent;	/* 基线到行顶的距离 */
int fm_line_height;	/* 行高 */
static uint16_t fm_w, fm_h;
extern int bpp;

void px(int x, int y, unsigned r, unsigned g, unsigned b);
int is_text(const char *path);
int is_pdf(const char *path);
int show_pdf(char *path);
int show_text(char *path, uint16_t w, uint16_t h);

void fm_font_close(void)
{
	int i;

	for (i = 0; i < FM_LAST - FM_FIRST + 1; i++) {
		free(fm_glyphs[i].bitmap);
		memset(&fm_glyphs[i], 0, sizeof(fm_glyphs[i]));
	}
	if (fm_face) {
		FT_Done_Face(fm_face);
		fm_face = NULL;
	}
	if (fm_lib) {
		FT_Done_FreeType(fm_lib);
		fm_lib = NULL;
	}
	fm_px = 0;
	fm_ascent = 0;
	fm_line_height = 0;
}

/* 惰性缓存 ASCII 字形，按当前字号渲染一次 */
struct fm_glyph *fm_glyph_get(unsigned char c)
{
	struct fm_glyph *gl;
	FT_GlyphSlot slot;
	FT_Bitmap *bm;
	int row;

	if (c < FM_FIRST || c > FM_LAST)
		c = '?';
	gl = &fm_glyphs[c - FM_FIRST];
	if (gl->loaded)
		return gl;
	gl->loaded = 1;
	if (!fm_face || FT_Load_Char(fm_face, c, FT_LOAD_RENDER))
		return gl;
	slot = fm_face->glyph;
	bm = &slot->bitmap;
	gl->w = (int) bm->width;
	gl->h = (int) bm->rows;
	gl->left = slot->bitmap_left;
	gl->top = slot->bitmap_top;
	gl->advance = (slot->advance.x + 32) >> 6;
	if (!gl->w || !gl->h)
		return gl;
	gl->bitmap = malloc((size_t) gl->w * gl->h);
	if (!gl->bitmap) {
		gl->w = 0;
		gl->h = 0;
		return gl;
	}
	for (row = 0; row < gl->h; row++) {
		long off = bm->pitch >= 0 ? (long) row * bm->pitch
			: (long) (gl->h - 1 - row) * -bm->pitch;
		memcpy(gl->bitmap + (size_t) row * gl->w, bm->buffer + off, gl->w);
	}
	return gl;
}

/* 单个字符的步进（越界字符按 '?' 处理） */
int fm_advance(unsigned char c)
{
	int adv = fm_glyph_get(c)->advance;
	return adv > 0 ? adv : 1;
}

int fm_font_load(void)
{
	const char *env, *path = NULL;
	int px = FM_FONT_PX * FBM_FONT_SCALE;
	int i;

	if (fm_face && px == fm_px)
		return 0;
	fm_font_close();
	if (FT_Init_FreeType(&fm_lib))
		return 1;
	fm_face = NULL;
	env = getenv("EREADER_FONT");
	if (env && env[0]) {
		if (!FT_New_Face(fm_lib, env, 0, &fm_face))
			path = env;
	} else {
		for (i = 0; font_paths[i]; i++)
			if (!FT_New_Face(fm_lib, font_paths[i], 0, &fm_face)) {
				path = font_paths[i];
				break;
			}
	}
	if (!path || !fm_face ||
			FT_Set_Pixel_Sizes(fm_face, 0, px)) {
		fprintf(stderr, "cannot load font\n");
		fm_font_close();
		return 1;
	}
	fm_px = px;
	fm_ascent = (fm_face->size->metrics.ascender + 40) >> 6;
	fm_line_height = (fm_face->size->metrics.height + 40) >> 6;
	return 0;
}

int fm_font_acquire(void)
{
	if (fm_font_load())
		return 1;
	fm_font_users++;
	return 0;
}

void fm_font_release(void)
{
	if (fm_font_users > 0 && --fm_font_users == 0)
		fm_font_close();
}

/* 按字形灰度覆盖值把前景色混合到帧缓冲（逻辑坐标，绘制时转 90 度） */
static void fm_blend_px(int x, int y, int a, unsigned r, unsigned g, unsigned b)
{
	char *d;
	unsigned br, bg, bb;
	int sx = y, sy = fm_w - 1 - x;	/* 屏幕坐标 */

	if (x < 0 || y < 0 || x >= fm_w || y >= fm_h)
		return;
	if (a >= 255) {
		px(sx, sy, r, g, b);
		return;
	}
	d = fb_mem(sy) + (size_t) sx * bpp;
	fb_get(d, &br, &bg, &bb);
	px(sx, sy, (r * a + br * (255 - a)) / 255,
		(g * a + bg * (255 - a)) / 255,
		(b * a + bb * (255 - a)) / 255);
}

/* y 为文字行顶部，返回绘制后的横向位置 */
int fm_char(int x, int y, unsigned char c, unsigned r, unsigned g, unsigned b)
{
	struct fm_glyph *gl = fm_glyph_get(c);
	int bx = x + gl->left;
	int by = y + fm_ascent - gl->top;
	int row, col;

	for (row = 0; gl->bitmap && row < gl->h; row++) {
		const unsigned char *src = gl->bitmap + (size_t) row * gl->w;
		for (col = 0; col < gl->w; col++)
			if (src[col])
				fm_blend_px(bx + col, by + row, src[col], r, g, b);
	}
	return gl->advance;
}

int fm_text(int x, int y, const char *s, unsigned r, unsigned g, unsigned b)
{
	while (*s) {
		unsigned char c = (unsigned char) *s++;
		if (c >= 0x80) {
			while (((unsigned char) *s & 0xc0) == 0x80)
				s++;
			c = '?';
		}
		x += fm_char(x, y, c, r, g, b);
	}
	return x;
}

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
		if (snprintf(full, sizeof(full), "%s%s%s", path,
				strcmp(path, "/") ? "/" : "", de->d_name) >= (int) sizeof(full))
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
	int n;

	if (v->n <= 0)
		return 1;
	n = snprintf(next, sizeof(next), "%s%s%s", v->path,
			strcmp(v->path, "/") ? "/" : "", v->ents[v->sel].name);
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
		show_pdf(next);	/* 打开失败时 show_pdf 自己会报错 */
		return 0;
	}
	if (!is_text(next))
		return 1;
	show_text(next, fm_w, fm_h);
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
	int pad = 4 * FBM_FONT_SCALE;
	int line_h = fm_line_height + 2 * FBM_FONT_SCALE;
	int header_h = fm_line_height + 8 * FBM_FONT_SCALE;
	int y0 = header_h + pad / 2;
	int visible = (fm_h - y0 - pad) / line_h;	// 可显示条目数

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

	fill_rect(0, 0, fm_h, fm_w, UI_BG, UI_BG, UI_BG);	// 填充背景
	fm_text(pad, pad, v->path, UI_ICON, UI_ICON, UI_ICON);	// 地址栏
	fill_rect(header_h, 0, FBM_FONT_SCALE, fm_w, UI_BORDER, UI_BORDER, UI_BORDER);	// 分隔线

	if (!v->n) {
		fm_text(pad, header_h + FBM_FONT_SCALE, "(empty)", UI_BORDER, UI_BORDER, UI_BORDER);
		return;
	}
	for (int i = v->top; i < v->n && i < v->top + visible; i++) {
		const struct fm_entry *e = &v->ents[i];
		int y = y0 + (i - v->top) * line_h;
		unsigned fg = (i == v->sel) ? UI_BG : UI_ICON;
		unsigned bg = (i == v->sel) ? UI_ICON : UI_BG;

		if (i == v->sel)
			fill_rect(y, 0, line_h, fm_w, bg, bg, bg);
		int x = fm_text(pad, y + FBM_FONT_SCALE, e->name, fg, fg, fg);
		if (e->isdir)
			fm_char(x, y + FBM_FONT_SCALE, '/', fg, fg, fg);
	}
}

int show_filemgr(char *path, uint16_t w, uint16_t h)
{
	struct fm_view v;
	struct stat st;
	int c, retval = EXIT_FAILURE;

	fm_w = h;	// 逆时针旋转90度
	fm_h = w;

	if (!path || !path[0])	// 参数错误
		return EXIT_FAILURE;
	if (stat(path, &st) != 0)	// 查不到状态
		return EXIT_FAILURE;
	if (!S_ISDIR(st.st_mode))	// 不是文件夹
		return EXIT_FAILURE;

	memset(&v, 0, sizeof(v));
	if (fm_font_acquire())
		return EXIT_FAILURE;

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
			if (v.sel + 1 < v.n) {
				v.sel++;
				fm_draw(&v);
			}
			break;
		case 'k':
			if (v.sel > 0) {
				v.sel--;
				fm_draw(&v);
			}
			break;
		default:
			continue;
		}
	}
	retval = EXIT_SUCCESS;
out:
	free(v.ents);
	fm_font_release();
	return retval;
}
