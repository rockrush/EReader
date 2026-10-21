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

#include "draw.h"
#include "fontlist.h"

#define FM_FONT_PX	12	/* 基准字号（像素），按屏幕缩放倍数放大 */
#define FM_MAX_FACES	48	/* 主字体 + 各文字系统的后备字体 */
#define FM_MAX_GLYPHS	8192	/* 字形缓存上限，超出时整体清空 */
#define FM_HASH_SIZE	1024
#define FM_MAX_PX	127

struct fm_glyph {
	unsigned int cp;	/* Unicode 码点 */
	int px;			/* 字号（像素） */
	int w, h;		/* 位图尺寸 */
	int left, top;		/* 位图相对基线的偏移 */
	int advance;		/* 步进（像素） */
	unsigned char *bitmap;	/* 8 位灰度位图，行宽为 w */
	struct fm_glyph *next;
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
static FT_Face fm_faces[FM_MAX_FACES];
static int fm_nfaces;
static int fm_face_px[FM_MAX_FACES];	/* 各字体当前设置的字号 */
static struct fm_glyph *fm_hash[FM_HASH_SIZE];
static int fm_nglyphs;
static int fm_ascent_px[FM_MAX_PX + 1];	/* 按字号缓存的基线偏移 */
static int fm_height_px[FM_MAX_PX + 1];	/* 按字号缓存的行高 */
static uint16_t fm_w, fm_h;
extern int bpp;

/* 状态栏线程和阅读线程共用字体与字形缓存（FT_Face 的字号是有状态的），
 * 用递归锁串行化访问：取字形时会内层再取行高/字号 */
static pthread_mutex_t fm_lock;
static pthread_once_t fm_lock_once = PTHREAD_ONCE_INIT;

static void fm_lock_init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&fm_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void fm_lock_take(void)
{
	pthread_once(&fm_lock_once, fm_lock_init);
	pthread_mutex_lock(&fm_lock);
}

static void fm_lock_drop(void)
{
	pthread_mutex_unlock(&fm_lock);
}

void px(int x, int y, unsigned r, unsigned g, unsigned b);
int is_text(const char *path);
int is_pdf(const char *path);
int show_pdf(char *path, uint16_t w, uint16_t h);
int show_text(char *path, uint16_t w, uint16_t h);

static void fm_glyphs_flush(void)
{
	int i;
	struct fm_glyph *gl, *next;

	for (i = 0; i < FM_HASH_SIZE; i++) {
		for (gl = fm_hash[i]; gl; gl = next) {
			next = gl->next;
			free(gl->bitmap);
			free(gl);
		}
		fm_hash[i] = NULL;
	}
	fm_nglyphs = 0;
}

static void fm_font_close(void)
{
	int i;

	fm_glyphs_flush();
	for (i = 0; i < fm_nfaces; i++)
		FT_Done_Face(fm_faces[i]);
	fm_nfaces = 0;
	if (fm_lib) {
		FT_Done_FreeType(fm_lib);
		fm_lib = NULL;
	}
	memset(fm_face_px, 0, sizeof(fm_face_px));
	memset(fm_ascent_px, 0, sizeof(fm_ascent_px));
	memset(fm_height_px, 0, sizeof(fm_height_px));
}

static int fm_clamp_px(int px)
{
	if (px < 1)
		return 1;
	if (px > FM_MAX_PX)
		return FM_MAX_PX;
	return px;
}

/* 切换某个字体的字号；已缓存的其他字号字形不受影响 */
static int fm_set_px(int face, int px)
{
	if (face < 0 || face >= fm_nfaces)
		return 1;
	if (fm_face_px[face] == px)
		return 0;
	if (FT_Set_Pixel_Sizes(fm_faces[face], 0, px))
		return 1;
	fm_face_px[face] = px;
	return 0;
}

/* 行高（像素），同时缓存该字号的基线偏移 */
int fm_line_height(int px)
{
	FT_Size_Metrics *m;
	int h;

	px = fm_clamp_px(px);
	fm_lock_take();
	if (fm_height_px[px]) {
		h = fm_height_px[px];
	} else if (!fm_nfaces || fm_set_px(0, px)) {
		h = px;
	} else {
		m = &fm_faces[0]->size->metrics;
		fm_ascent_px[px] = (m->ascender + 40) >> 6;
		fm_height_px[px] = (m->height + 40) >> 6;
		h = fm_height_px[px] ? fm_height_px[px] : px;
	}
	fm_lock_drop();
	return h;
}

static int fm_ascent(int px)
{
	int a;

	px = fm_clamp_px(px);
	fm_line_height(px);
	fm_lock_take();
	a = fm_ascent_px[px];
	fm_lock_drop();
	return a;
}

/* 依次尝试主字体和后备字体，把 cp 渲染到 gl 中；返回 0 表示成功 */
static int fm_load_glyph(struct fm_glyph *gl, unsigned int cp, int px)
{
	FT_GlyphSlot slot;
	FT_Bitmap *bm;
	int i, row;

	for (i = 0; i < fm_nfaces; i++) {
		if (!FT_Get_Char_Index(fm_faces[i], cp))
			continue;	/* 该字体没有此字符 */
		if (fm_set_px(i, px) || FT_Load_Char(fm_faces[i], cp, FT_LOAD_RENDER))
			continue;
		slot = fm_faces[i]->glyph;
		bm = &slot->bitmap;
		gl->w = (int) bm->width;
		gl->h = (int) bm->rows;
		gl->left = slot->bitmap_left;
		gl->top = slot->bitmap_top;
		gl->advance = (slot->advance.x + 32) >> 6;
		if (!gl->w || !gl->h)
			return 0;
		gl->bitmap = malloc((size_t) gl->w * gl->h);
		if (!gl->bitmap) {
			gl->w = 0;
			gl->h = 0;
			return 0;
		}
		for (row = 0; row < gl->h; row++) {
			long off = bm->pitch >= 0 ? (long) row * bm->pitch
				: (long) (gl->h - 1 - row) * -bm->pitch;
			memcpy(gl->bitmap + (size_t) row * gl->w, bm->buffer + off, gl->w);
		}
		return 0;
	}
	return 1;
}

static struct fm_glyph *fm_glyph_make(unsigned int cp, int px)
{
	struct fm_glyph *gl = calloc(1, sizeof(*gl));

	if (!gl)
		return NULL;
	gl->cp = cp;
	gl->px = px;
	if (fm_load_glyph(gl, cp, px) && cp != '?')
		fm_load_glyph(gl, '?', px);	/* 未收录的字符显示为 '?' */
	if (gl->advance <= 0)
		gl->advance = px > 1 ? px / 2 : 1;
	return gl;
}

/* 按 (码点, 字号) 惰性缓存字形 */
static struct fm_glyph *fm_glyph_get(unsigned int cp, int px)
{
	unsigned int h;
	struct fm_glyph *gl;

	px = fm_clamp_px(px);
	fm_lock_take();
	h = (cp * 31u + (unsigned int) px) & (FM_HASH_SIZE - 1);
	for (gl = fm_hash[h]; gl; gl = gl->next)
		if (gl->cp == cp && gl->px == px) {
			fm_lock_drop();
			return gl;
		}
	if (fm_nglyphs >= FM_MAX_GLYPHS)
		fm_glyphs_flush();
	gl = fm_glyph_make(cp, px);
	if (gl) {
		gl->next = fm_hash[h];
		fm_hash[h] = gl;
		fm_nglyphs++;
	}
	fm_lock_drop();
	return gl;
}

/* 解码一个 UTF-8 码点并前移指针，非法序列按 '?' 处理 */
unsigned int fm_utf8(const char **sp)
{
	const unsigned char *s = (const unsigned char *) *sp;
	unsigned int cp, min;
	int n, i;

	if (s[0] < 0x80) {
		*sp += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0) {
		cp = s[0] & 0x1f;
		n = 1;
		min = 0x80;
	} else if ((s[0] & 0xf0) == 0xe0) {
		cp = s[0] & 0x0f;
		n = 2;
		min = 0x800;
	} else if ((s[0] & 0xf8) == 0xf0) {
		cp = s[0] & 0x07;
		n = 3;
		min = 0x10000;
	} else {
		*sp += 1;
		return '?';
	}
	for (i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*sp += 1;
			return '?';
		}
		cp = (cp << 6) | (s[i] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
		*sp += 1;
		return '?';
	}
	*sp += n + 1;
	return cp;
}

/* 单个字符的步进（越界字符按 '?' 处理） */
int fm_advance(unsigned int cp, int px)
{
	struct fm_glyph *gl;
	int adv;

	fm_lock_take();
	gl = fm_glyph_get(cp, px);
	adv = gl && gl->advance > 0 ? gl->advance : 1;
	fm_lock_drop();
	return adv;
}

static int fm_load_face(const char *path, int index)
{
	FT_Face face;

	if (fm_nfaces >= FM_MAX_FACES)
		return 1;
	if (FT_New_Face(fm_lib, path, index, &face))
		return 1;
	FT_Select_Charmap(face, FT_ENCODING_UNICODE);
	fm_faces[fm_nfaces] = face;
	fm_face_px[fm_nfaces] = 0;
	fm_nfaces++;
	return 0;
}

int fm_font_load(void)
{
	struct font_candidate fc;
	int n = 0;

	if (fm_nfaces)
		return 0;
	if (FT_Init_FreeType(&fm_lib))
		return 1;
	if (font_list_open()) {
		while (fm_nfaces < FM_MAX_FACES && font_list_next(&fc))
			if (!fm_load_face(fc.path, fc.index))
				n++;
		font_list_close();
	}
	if (!n) {
		fprintf(stderr, "cannot load font\n");
		fm_font_close();
		return 1;
	}
	return 0;
}

int fm_font_acquire(void)
{
	int ret;

	fm_lock_take();
	ret = fm_font_load();
	if (!ret)
		fm_font_users++;
	fm_lock_drop();
	return ret;
}

void fm_font_release(void)
{
	fm_lock_take();
	if (fm_font_users > 0 && --fm_font_users == 0)
		fm_font_close();
	fm_lock_drop();
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
int fm_char(int x, int y, unsigned int cp, int px, unsigned r, unsigned g, unsigned b)
{
	struct fm_glyph *gl;
	int bx, by, row, col, adv;

	fm_lock_take();
	gl = fm_glyph_get(cp, px);
	if (!gl) {
		fm_lock_drop();
		return 1;
	}
	bx = x + gl->left;
	by = y + fm_ascent(px) - gl->top;
	for (row = 0; gl->bitmap && row < gl->h; row++) {
		const unsigned char *src = gl->bitmap + (size_t) row * gl->w;
		for (col = 0; col < gl->w; col++)
			if (src[col])
				fm_blend_px(bx + col, by + row, src[col], r, g, b);
	}
	adv = gl->advance > 0 ? gl->advance : 1;
	fm_lock_drop();
	return adv;
}

/* 按字形覆盖率把一个像素混合到屏幕坐标（不转置，供竖排等非页面区域使用） */
static void fm_blend_screen(int x, int y, int a, unsigned r, unsigned g, unsigned b)
{
	unsigned br, bg, bb;

	if (a >= 255) {
		px(x, y, r, g, b);
		return;
	}
	fb_get(fb_mem(y) + (size_t) x * bpp, &br, &bg, &bb);
	px(x, y, (r * a + br * (255 - a)) / 255,
	   (g * a + bg * (255 - a)) / 255,
	   (b * a + bb * (255 - a)) / 255);
}

/* 竖排单字：字形逆时针旋转 90°，居中画在 (x, y) 起、cell x cell 的方格内。
 * 旋转后位图宽高互换，行优先的 (col, row) 映射到 (row, w-1-col) */
static void fm_char_btt(int x, int y, unsigned int cp, int size, int cell,
			unsigned r, unsigned g, unsigned b)
{
	struct fm_glyph *gl;
	int ox, oy, row, col;

	fm_lock_take();
	gl = fm_glyph_get(cp, size);
	if (!gl) {
		fm_lock_drop();
		return;
	}
	ox = x + (cell - gl->h) / 2;
	oy = y + (cell - gl->w) / 2;
	for (row = 0; gl->bitmap && row < gl->h; row++) {
		const unsigned char *src = gl->bitmap + (size_t) row * gl->w;
		for (col = 0; col < gl->w; col++)
			if (src[col])
				fm_blend_screen(ox + row, oy + gl->w - 1 - col,
						src[col], r, g, b);
	}
	fm_lock_drop();
}

/* 竖排文字：字符旋转 90° 后自底向上书写。
 * (x, y) 为最下方字符方格的左上角，cell 为方格边长，返回整列顶端 y */
int fm_text_btt(int x, int y, const char *s, size_t len, int size, int cell,
		unsigned r, unsigned g, unsigned b)
{
	const char *p = s, *end = s + len;

	while (p < end && *p) {
		fm_char_btt(x, y, fm_utf8(&p), size, cell, r, g, b);
		y -= cell;
	}
	return y + cell;
}

/* 绘制 len 字节的 UTF-8 文本 */
int fm_text_n(int x, int y, const char *s, size_t len, int px, unsigned r, unsigned g, unsigned b)
{
	const char *p = s, *end = s + len;
	while (p < end && *p) {
		unsigned int cp = fm_utf8(&p);
		x += fm_char(x, y, cp, px, r, g, b);
	}
	return x;
}

int fm_text(int x, int y, const char *s, int px, unsigned r, unsigned g, unsigned b)
{
	return fm_text_n(x, y, s, strlen(s), px, r, g, b);
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
		show_pdf(next, fm_h, fm_w);
		return 0;
	}
	if (!is_text(next))
		return 1;
	show_text(next, fm_h, fm_w);
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
	int line_h = fm_line_height(px) + 2 * FBM_FONT_SCALE;
	int header_h = fm_line_height(px) + 8 * FBM_FONT_SCALE;
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
	fm_text(pad, pad, v->path, px, UI_ICON, UI_ICON, UI_ICON);	// 地址栏
	fill_rect(header_h, 0, FBM_FONT_SCALE, fm_w, UI_BORDER, UI_BORDER, UI_BORDER);	// 分隔线

	if (!v->n) {
		fm_text(pad, header_h + FBM_FONT_SCALE, "(empty)", px, UI_BORDER, UI_BORDER, UI_BORDER);
		return;
	}
	for (int i = v->top; i < v->n && i < v->top + visible; i++) {
		const struct fm_entry *e = &v->ents[i];
		int y = y0 + (i - v->top) * line_h;
		unsigned fg = (i == v->sel) ? UI_BG : UI_ICON;
		unsigned bg = (i == v->sel) ? UI_ICON : UI_BG;

		if (i == v->sel)
			fill_rect(y, 0, line_h, fm_w, bg, bg, bg);
		int x = fm_text(pad, y + FBM_FONT_SCALE, e->name, px, fg, fg, fg);
		if (e->isdir)
			fm_text(x, y + FBM_FONT_SCALE, "/", px, fg, fg, fg);
	}
}

void fm_set_view(uint16_t w, uint16_t h) {
	fm_w = h;
	fm_h = w;
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
