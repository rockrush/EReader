/* 系统字体发现：用 fontconfig 按系统配置挑选字体 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include "ereader.h"
#include "font.h"
#include "draw.h"

extern int bpp;

#define LIST_MAX	64	/* 候选字体上限 */
#define SCAN_DEPTH	4	/* 目录扫描深度 */
#define MAX_FACES	48	/* 主字体 + 各文字系统的后备字体 */
#define FM_MAX_GLYPHS	8192	/* 字形缓存上限，超出时整体清空 */
#define FM_HASH_SIZE	1024
#define FM_MAX_PX	127


struct font_entry {
	char *path;
	char *family;	/* fontconfig 家族名，未知时为 NULL */
	int index;
};

struct font_glyph {
	unsigned int cp;	/* Unicode 码点 */
	int px;			/* 字号（像素） */
	int w, h;		/* 位图尺寸 */
	int left, top;		/* 位图相对基线的偏移 */
	int advance;		/* 步进（像素） */
	unsigned char *bitmap;	/* 8 位灰度位图，行宽为 w */
	struct font_glyph *next;
};

struct font_candidate {
	const char *path;
	int index;      /* 字体集合（.ttc/.otc）内的序号，单文件字体为 0 */
};

static struct font_entry *fonts;
static int nfonts, nfaces, nglyphs;
static FT_Library font_lib;
static FT_Face font_faces[MAX_FACES];
static int font_face_px[MAX_FACES];	/* 各字体当前设置的字号 */
static struct font_glyph *font_hash[FM_HASH_SIZE];
static int font_ascent_px[FM_MAX_PX + 1];	/* 按字号缓存的基线偏移 */
static int font_height_px[FM_MAX_PX + 1];	/* 按字号缓存的行高 */

/* 该字体家族是否已经收录（同一家族另有粗体、斜体等文件） */
static int list_has_family(const char *family)
{
	if (!family)
		return 0;
	for (int i = 0; i < nfonts; i++)
		if (fonts[i].family && !strcmp(fonts[i].family, family))
			return 1;
	return 0;
}

static void list_add(const char *path, const char *family, int index)
{
	struct font_entry *e;
	char *dup, *fam = NULL;
	int i;

	if (!path || !path[0] || nfonts >= LIST_MAX)
		return;
	for (i = 0; i < nfonts; i++)
		if (fonts[i].index == index && !strcmp(fonts[i].path, path))
			return;
	if (!(dup = strdup(path)))
		return;
	if (family && !(fam = strdup(family))) {
		free(dup);
		return;
	}
	e = realloc(fonts, (size_t) (nfonts + 1) * sizeof(*fonts));
	if (!e) {
		free(dup);
		free(fam);
		return;
	}
	fonts = e;
	fonts[nfonts].path = dup;
	fonts[nfonts].family = fam;
	fonts[nfonts].index = index;
	nfonts++;
}

/* 环境变量可写多个字体，用冒号分隔，排在最前面作为首选字体 */
static void env_list(void)
{
	const char *env = getenv("EREADER_FONT");
	char *copy, *p, *save = NULL;

	if (!env || !env[0] || !(copy = strdup(env)))
		return;
	for (p = strtok_r(copy, ":", &save); p; p = strtok_r(NULL, ":", &save))
		list_add(p, NULL, 0);
	free(copy);
}

static bool fc_ready;

static void fc_add_pattern(const FcPattern *pat)
{
	FcChar8 *file, *family = NULL;
	int index = 0;

	if (FcPatternGetString(pat, FC_FILE, 0, &file) != FcResultMatch)
		return;
	/* 合并的 .ttc/.otc 里不同语言在同一文件的不同序号上 */
	FcPatternGetInteger(pat, FC_INDEX, 0, &index);
	FcPatternGetString(pat, FC_FAMILY, 0, &family);
	list_add((const char *) file, (const char *) family, index);
}

static FcPattern *fc_pattern(const char *family)
{
	FcPattern *pat = FcPatternCreate();

	if (!pat)
		return NULL;
	if (family)
		FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *) family);
	FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
	FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	return pat;
}

/* 各文字系统的样本字符，附 fontconfig 语言标签以便按该语言挑选字体。
 * 越常见的语言排得越前：字体面数有上限时后面较冷门的文字系统先被舍弃。
 * primary 表示常见等宽/无衬线字体一般已收录：主字体覆盖了就不再另找后备 */
static const struct script_sample {
	FcChar32 cp;
	const char *lang;
	bool primary;
} samples[] = {
	{ 0x00e9, "fr",		true },	/* 拉丁字母（法语/西班牙语等） */
	{ 0x03b1, "el",		true },	/* 希腊文 */
	{ 0x0416, "ru",		true },	/* 西里尔文（俄语等） */
	{ 0x2605, NULL,		true },	/* 符号（★） */
	{ 0x4e2d, "zh-cn",	false }, /* 简体中文 */
	{ 0x6f22, "zh-tw",	false }, /* 繁体中文 */
	{ 0x3042, "ja",		false }, /* 日文平假名 */
	{ 0x30a2, "ja",		false }, /* 日文片假名 */
	{ 0xd55c, "ko",		false }, /* 韩文谚文 */
	{ 0x0627, "ar",		false }, /* 阿拉伯文 */
	{ 0x05d0, "he",		false }, /* 希伯来文 */
	{ 0x0915, "hi",		false }, /* 天城文（印地语/马拉地语等） */
	{ 0x0e01, "th",		false }, /* 泰文 */
	{ 0x0995, "bn",		false }, /* 孟加拉文 */
	{ 0x0a15, "pa",		false }, /* 古木基文（旁遮普语） */
	{ 0x0a95, "gu",		false }, /* 古吉拉特文 */
	{ 0x0b15, "or",		false }, /* 奥里亚文 */
	{ 0x0b95, "ta",		false }, /* 泰米尔文 */
	{ 0x0c15, "te",		false }, /* 泰卢固文 */
	{ 0x0c95, "kn",		false }, /* 卡纳达文 */
	{ 0x0d15, "ml",		false }, /* 马拉雅拉姆文 */
	{ 0x0d9a, "si",		false }, /* 僧伽罗文 */
	{ 0x0e81, "lo",		false }, /* 老挝文 */
	{ 0x0f40, "bo",		false }, /* 藏文 */
	{ 0x1000, "my",		false }, /* 缅甸文 */
	{ 0x1780, "km",		false }, /* 高棉文 */
	{ 0x067e, "fa",		false }, /* 波斯文/乌尔都文 */
	{ 0x0561, "hy",		false }, /* 亚美尼亚文 */
	{ 0x10d0, "ka",		false }, /* 格鲁吉亚文 */
	{ 0x1200, "am",		false }, /* 埃塞俄比亚文（阿姆哈拉语等） */
	{ 0x0710, "syr",	false }, /* 叙利亚文 */
	{ 0x0780, "dv",		false }, /* 塔安那文（迪维希语） */
	{ 0x2d30, "tzm",	false }, /* 提非纳文（柏柏尔语） */
	{ 0x07ca, "nqo",	false }, /* 西非 N'Ko 文 */
	{ 0x13a0, "chr",	false }, /* 切罗基文 */
	{ 0xa500, "vai",	false }, /* 瓦伊文 */
};

#define NSAMPLES	(sizeof(samples) / sizeof(samples[0]))

/* 该字体是否收录了 cp */
static int fc_pattern_has(const FcPattern *pat, FcChar32 cp)
{
	FcCharSet *cs;

	return FcPatternGetCharSet(pat, FC_CHARSET, 0, &cs) == FcResultMatch &&
	       FcCharSetHasChar(cs, cp);
}

/* 标记这款字体已经覆盖的样本，省掉后续重复查询和重复加载。
 * primary_only 为真时只认主字体该负责的常见文字系统 */
static void fc_mark_covered(const FcPattern *pat, bool *covered, bool primary_only)
{
	size_t i;

	for (i = 0; i < NSAMPLES; i++)
		if (!covered[i] && (!primary_only || samples[i].primary) &&
		    fc_pattern_has(pat, samples[i].cp))
			covered[i] = true;
}

/* 取系统为该字体族选出的首选字体。mono 为真表示这是等宽正文字体：
 * 它碰巧收录的少量非拉丁字符字型通常不全，只标记常见文字系统 */
static void fc_add_family(const char *family, bool mono, bool *covered)
{
	FcResult res;
	FcPattern *pat = fc_pattern(family);
	FcPattern *match;

	if (!pat)
		return;
	match = FcFontMatch(NULL, pat, &res);
	if (match) {
		fc_add_pattern(match);
		fc_mark_covered(match, covered, mono);
		FcPatternDestroy(match);
	}
	FcPatternDestroy(pat);
}

/* 按该文字系统的语言挑一款能显示样本字符的字体，作为后备字体 */
static void fc_add_for_sample(const struct script_sample *sp, bool *covered)
{
	FcResult res;
	FcLangSet *lang;
	FcPattern *pat;
	FcCharSet *req;
	FcFontSet *set;
	int i;

	if (!(pat = fc_pattern(NULL)) || !(req = FcCharSetCreate())) {
		FcPatternDestroy(pat);
		return;
	}
	FcCharSetAddChar(req, sp->cp);
	FcPatternAddCharSet(pat, FC_CHARSET, req);
	if (sp->lang && (lang = FcLangSetCreate())) {
		FcLangSetAdd(lang, (const FcChar8 *) sp->lang);
		FcPatternAddLangSet(pat, FC_LANG, lang);
		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);
		FcLangSetDestroy(lang);
	}
	set = FcFontSort(NULL, pat, FcFalse, NULL, &res);
	for (i = 0; set && i < set->nfont; i++) {
		FcChar8 *family = NULL;

		if (!fc_pattern_has(set->fonts[i], sp->cp))
			continue;
		/* 主字体碰巧收录了少量该文字系统的字符，字型通常不全，
		 * 所以跳过同一家族已收录的字体（含其粗体、斜体等变体），
		 * 另找一款该语言的专用字体 */
		FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &family);
		if (list_has_family((const char *) family))
			continue;
		fc_add_pattern(set->fonts[i]);
		fc_mark_covered(set->fonts[i], covered, false);
		covered[sp - samples] = true;	/* 至少这一款可用 */
		break;
	}
	if (set)
		FcFontSetDestroy(set);
	FcPatternDestroy(pat);
	FcCharSetDestroy(req);
}

static void system_list(void)
{
	static bool covered[NSAMPLES];
	size_t i;

	if (!fc_ready)
		fc_ready = FcInit();
	if (!fc_ready)
		return;
	/* 主字体：系统首选的等宽字体，其次无衬线字体 */
	fc_add_family("monospace", true, covered);
	fc_add_family("sans-serif", false, covered);
	/* 其余文字系统各挑一款字体作为后备 */
	for (i = 0; i < NSAMPLES && nfonts < LIST_MAX; i++)
		if (!covered[i])
			fc_add_for_sample(&samples[i], covered);
}

/* 状态栏线程和阅读线程共用字体与字形缓存（FT_Face 的字号是有状态的），
 * 用递归锁串行化访问：取字形时会内层再取行高/字号 */
static pthread_mutex_t font_lock;
static pthread_once_t font_lock_once = PTHREAD_ONCE_INIT;

static void font_lock_init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&font_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

void font_lock_take(void)
{
	pthread_once(&font_lock_once, font_lock_init);
	pthread_mutex_lock(&font_lock);
}

void font_lock_drop(void)
{
	pthread_mutex_unlock(&font_lock);
}

static int clamp_px(int px)
{
	if (px < 1)
		return 1;
	if (px > FM_MAX_PX)
		return FM_MAX_PX;
	return px;
}

/* 切换某个字体的字号；已缓存的其他字号字形不受影响 */
static int set_px(int face, int px)
{
	if (face < 0 || face >= nfaces)
		return 1;
	if (font_face_px[face] == px)
		return 0;
	if (FT_Set_Pixel_Sizes(font_faces[face], 0, px))
		return 1;
	font_face_px[face] = px;
	return 0;
}

/* 依次尝试主字体和后备字体，把 cp 渲染到 gl 中；返回 0 表示成功 */
static int load_glyph(struct font_glyph *gl, unsigned int cp, int px)
{
	FT_GlyphSlot slot;
	FT_Bitmap *bm;
	int i, row;

	for (i = 0; i < nfaces; i++) {
		if (!FT_Get_Char_Index(font_faces[i], cp))
			continue;	/* 该字体没有此字符 */
		if (set_px(i, px) || FT_Load_Char(font_faces[i], cp, FT_LOAD_RENDER))
			continue;
		slot = font_faces[i]->glyph;
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

static void font_glyphs_flush(void)
{
	struct font_glyph *gl, *next;

	for (int i = 0; i < FM_HASH_SIZE; i++) {
		for (gl = font_hash[i]; gl; gl = next) {
			next = gl->next;
			free(gl->bitmap);
			free(gl);
		}
		font_hash[i] = NULL;
	}
	nglyphs = 0;
}

static struct font_glyph *font_glyph_make(unsigned int cp, int px)
{
	struct font_glyph *gl = calloc(1, sizeof(*gl));

	if (!gl)
		return NULL;
	gl->cp = cp;
	gl->px = px;
	if (load_glyph(gl, cp, px) && cp != '?')
		load_glyph(gl, '?', px);	/* 未收录的字符显示为 '?' */
	if (gl->advance <= 0)
		gl->advance = px > 1 ? px / 2 : 1;
	return gl;
}

/* 按 (码点, 字号) 惰性缓存字形 */
static struct font_glyph *font_glyph_get(unsigned int cp, int px)
{
	unsigned int h;
	struct font_glyph *gl;

	px = clamp_px(px);
	font_lock_take();
	h = (cp * 31u + (unsigned int) px) & (FM_HASH_SIZE - 1);
	for (gl = font_hash[h]; gl; gl = gl->next)
		if (gl->cp == cp && gl->px == px) {
			font_lock_drop();
			return gl;
		}
	if (nglyphs >= FM_MAX_GLYPHS)
		font_glyphs_flush();
	gl = font_glyph_make(cp, px);
	if (gl) {
		gl->next = font_hash[h];
		font_hash[h] = gl;
		nglyphs++;
	}
	font_lock_drop();
	return gl;
}

/* 单个字符的步进（越界字符按 '?' 处理） */
int font_advance(unsigned int cp, int px)
{
	struct font_glyph *gl;
	int adv;

	font_lock_take();
	gl = font_glyph_get(cp, px);
	adv = gl && gl->advance > 0 ? gl->advance : 1;
	font_lock_drop();
	return adv;
}

/* 按字形灰度覆盖值把前景色混合到帧缓冲（逻辑坐标，绘制时转 90 度） */
void blend_px(int x, int y, int a, unsigned r, unsigned g, unsigned b)
{
	char *d;
	unsigned br, bg, bb;
	uint16_t vw = cfg.view_w, vh = cfg.scr_w;
	int sx = y, sy = vw - 1 - x;	/* 屏幕坐标 */

	if (x < -cfg.ui_w || y < 0 || x >= vw || y >= vh) {
		printf("blend_px: %d, %d\n", x, y);
		return;
	}
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

/* 行高（像素），同时缓存该字号的基线偏移 */
int font_line_height(int px)
{
	FT_Size_Metrics *m;
	int h;

	px = clamp_px(px);
	font_lock_take();
	if (font_height_px[px]) {
		h = font_height_px[px];
	} else if (!nfaces || set_px(0, px)) {
		h = px;
	} else {
		m = &font_faces[0]->size->metrics;
		font_ascent_px[px] = (m->ascender + 40) >> 6;
		font_height_px[px] = (m->height + 40) >> 6;
		h = font_height_px[px] ? font_height_px[px] : px;
	}
	font_lock_drop();
	return h;
}

static int font_ascent(int px)
{
	int a;
	px = clamp_px(px);
	font_line_height(px);
	font_lock_take();
	a = font_ascent_px[px];
	font_lock_drop();
	return a;
}

/* y 为文字行顶部，返回绘制后的横向位置 */
int font_char(int x, int y, unsigned int cp, int px, unsigned r, unsigned g, unsigned b)
{
	struct font_glyph *gl;
	int bx, by, row, col, adv;

	font_lock_take();
	gl = font_glyph_get(cp, px);
	if (!gl) {
		font_lock_drop();
		return 1;
	}
	bx = x + gl->left;
	by = y + font_ascent(px) - gl->top;
	for (row = 0; gl->bitmap && row < gl->h; row++) {
		const unsigned char *src = gl->bitmap + (size_t) row * gl->w;
		for (col = 0; col < gl->w; col++)
			if (src[col])
				blend_px(bx + col, by + row, src[col], r, g, b);
	}
	adv = gl->advance > 0 ? gl->advance : 1;
	font_lock_drop();
	return adv;
}

/* 解码一个 UTF-8 码点并前移指针，非法序列按 '?' 处理 */
unsigned int font_utf8(const char **sp)
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

/* 绘制 len 字节的 UTF-8 文本 */
int text_n(int x, int y, const char *s, size_t len, int px, unsigned r, unsigned g, unsigned b)
{
	const char *p = s, *end = s + len;
	while (p < end && *p) {
		unsigned int cp = font_utf8(&p);
		x += font_char(x, y, cp, px, r, g, b);
	}
	return x;
}

int text(int x, int y, const char *s, int px, unsigned r, unsigned g, unsigned b)
{
	return text_n(x, y, s, strlen(s), px, r, g, b);
}

static int font_load_face(const char *path, int index)
{
	FT_Face face;

	if (nfaces >= MAX_FACES)
		return 1;
	if (FT_New_Face(font_lib, path, index, &face))
		return 1;
	FT_Select_Charmap(face, FT_ENCODING_UNICODE);
	font_faces[nfaces] = face;
	font_face_px[nfaces] = 0;
	nfaces++;
	return 0;
}

void font_close(void)
{
	font_glyphs_flush();
	for (int i = 0; i < nfaces; i++)
		FT_Done_Face(font_faces[i]);
	nfaces = 0;
	if (font_lib) {
		FT_Done_FreeType(font_lib);
		font_lib = NULL;
	}
	memset(font_face_px, 0, sizeof(font_face_px));
	memset(font_ascent_px, 0, sizeof(font_ascent_px));
	memset(font_height_px, 0, sizeof(font_height_px));
}

int font_load(void)
{
	if (nfaces)
		return 0;
	if (FT_Init_FreeType(&font_lib))
		return 1;

	env_list();
	system_list();

	if (nfonts <= 0) {
		fprintf(stderr, "cannot load font\n");
		font_close();
		return 1;
	}

	while (nfaces < MAX_FACES) {
		for (int i = 0; i < nfonts; i++)
			font_load_face(fonts[i].path, fonts[i].index);
	}
	return 0;	// 字体发现的结果体积很小，缓存到进程退出，避免每次打开视图都重新查询
}
