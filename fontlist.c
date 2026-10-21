/*
 * 系统字体发现：优先用 fontconfig 按系统配置挑选字体，没有 fontconfig 时
 * 扫描常见字体目录。调用方无需知道任何字体路径。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include "fontlist.h"

#define LIST_MAX	64	/* 候选字体上限 */
#define SCAN_DEPTH	4	/* 目录扫描深度 */

#ifdef HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

struct font_entry {
	char *path;
	char *family;	/* fontconfig 家族名，未知时为 NULL */
	int index;
};

static struct font_entry *fonts;
static int nfonts;
static int cur;
static bool built;	/* 候选表只查一次，之后反复使用 */

#ifdef HAVE_FONTCONFIG
/* 该字体家族是否已经收录（同一家族另有粗体、斜体等文件） */
static int list_has_family(const char *family)
{
	int i;

	if (!family)
		return 0;
	for (i = 0; i < nfonts; i++)
		if (fonts[i].family && !strcmp(fonts[i].family, family))
			return 1;
	return 0;
}
#endif

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

#ifdef HAVE_FONTCONFIG

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

#else	/* !HAVE_FONTCONFIG */

static char **scan_paths;
static int nscan;

static int has_ci(const char *s, const char *word)
{
	size_t n = strlen(word);

	for (; *s; s++)
		if (!strncasecmp(s, word, n))
			return 1;
	return 0;
}

/* 常见文字系统，按常见程度排列：没有 fontconfig 时按文件名把常用语言排到前面，
 * 避免在字体面数上限内被冷门文字系统挤掉 */
static const char *script_names[] = {
	"cjk", "fallback", "unifont",	/* 覆盖宽，优先作为后备 */
	"arabic", "hebrew", "devanagari", "thai", "bengali", "tamil",
	"telugu", "kannada", "malayalam", "gujarati", "gurmukhi", "oriya",
	"sinhala", "lao", "tibetan", "myanmar", "khmer", "georgian",
	"armenian", "ethiopic", "syriac", "thaana", "tifinagh", "nko",
	"cherokee", "vai",
};

#define NSCAN_SCRIPTS	(sizeof(script_names) / sizeof(script_names[0]))
#define SCAN_MONO	0
#define SCAN_SANS	(NSCAN_SCRIPTS + 1)

/* 文件名所属类别：等宽、某个文字系统，或无衬线；-1 表示未识别 */
static int scan_class(const char *path)
{
	const char *base = strrchr(path, '/');
	size_t i;

	base = base ? base + 1 : path;
	if (has_ci(base, "mono"))
		return SCAN_MONO;
	for (i = 0; i < NSCAN_SCRIPTS; i++)
		if (has_ci(base, script_names[i]))
			return (int) i + 1;
	if (has_ci(base, "sans"))
		return SCAN_SANS;
	return -1;
}

/* 越小越优先：等宽正文 > 覆盖宽的后备字体 > 常见语言专用字体 > 无衬线 > 其他；
 * 粗体/斜体等变体排在常规字重之后 */
static int font_rank(const char *path)
{
	const char *base = strrchr(path, '/');
	int cls = scan_class(path);
	int rank;

	base = base ? base + 1 : path;
	if (cls == SCAN_MONO)
		rank = 0;
	else if (cls >= 1 && cls <= 3)		/* cjk / fallback / unifont */
		rank = 10 + cls;
	else if (cls > 3 && cls <= (int) NSCAN_SCRIPTS)
		rank = 100 + cls;
	else if (cls == SCAN_SANS)
		rank = 1000;
	else
		rank = 2000;
	if (has_ci(base, "bold") || has_ci(base, "italic") || has_ci(base, "oblique") ||
	    has_ci(base, "light") || has_ci(base, "black") || has_ci(base, "condensed"))
		rank += 10000;
	else if (!has_ci(base, "regular") && !has_ci(base, "book"))
		rank++;
	return rank;
}

static int scan_cmp(const void *a, const void *b)
{
	const char *pa = *(char *const *) a, *pb = *(char *const *) b;
	int ra = font_rank(pa), rb = font_rank(pb);

	return ra != rb ? ra - rb : strcmp(pa, pb);
}

static int is_font_file(const char *name)
{
	const char *dot = strrchr(name, '.');

	/* 只收 Unicode 可用的轮廓字体；Type1 等老格式没有 Unicode cmap，跳过 */
	return dot && (!strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") ||
		       !strcasecmp(dot, ".ttc") || !strcasecmp(dot, ".otc"));
}

static void scan_add(const char *path)
{
	char **p = realloc(scan_paths, (size_t) (nscan + 1) * sizeof(*scan_paths));
	char *dup;

	if (!p)
		return;
	scan_paths = p;
	if (!(dup = strdup(path)))
		return;
	scan_paths[nscan++] = dup;
}

static void scan_dir(const char *dir, int depth)
{
	struct dirent *de;
	struct stat st;
	char full[PATH_MAX];
	DIR *d;

	if (depth > SCAN_DEPTH || !(d = opendir(dir)))
		return;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int) sizeof(full))
			continue;
		if (lstat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			scan_dir(full, depth + 1);
		else if (S_ISREG(st.st_mode) && is_font_file(de->d_name))
			scan_add(full);
	}
	closedir(d);
}

static void scan_home(void)
{
	const char *home = getenv("HOME");
	const char *data = getenv("XDG_DATA_HOME");
	char path[PATH_MAX];

	if (data && data[0]) {
		snprintf(path, sizeof(path), "%s/fonts", data);
		scan_dir(path, 0);
	} else if (home && home[0]) {
		snprintf(path, sizeof(path), "%s/.local/share/fonts", home);
		scan_dir(path, 0);
	}
	if (home && home[0]) {
		snprintf(path, sizeof(path), "%s/.fonts", home);
		scan_dir(path, 0);
	}
}

static void system_list(void)
{
	static const char *dirs[] = {
		"/usr/share/fonts",
		"/usr/local/share/fonts",
		"/usr/share/X11/fonts",
		"/system/fonts",	/* Android */
	};
	int used[NSCAN_SCRIPTS + 2];	/* 每类字体最多收两种，避免同一个字体族占满名额 */
	size_t i;
	int n;

	scan_home();
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		scan_dir(dirs[i], 0);
	if (!nscan)
		return;
	qsort(scan_paths, (size_t) nscan, sizeof(*scan_paths), scan_cmp);
	memset(used, 0, sizeof(used));
	for (n = 0; n < nscan; n++) {
		int cls = scan_class(scan_paths[n]);
		int max = cls >= 0 && cls <= 3 ? 2 : 1;	/* 等宽、宽覆盖字体多留一个后备 */

		if (cls < 0 || used[cls]++ < max)
			list_add(scan_paths[n], NULL, 0);
		free(scan_paths[n]);
	}
	free(scan_paths);
	scan_paths = NULL;
	nscan = 0;
}

#endif	/* HAVE_FONTCONFIG */

int font_list_open(void)
{
	if (!built) {
		env_list();
		system_list();
		built = true;
	}
	cur = 0;
	return nfonts > 0;
}

int font_list_next(struct font_candidate *fc)
{
	if (cur >= nfonts)
		return 0;
	fc->path = fonts[cur].path;
	fc->index = fonts[cur].index;
	cur++;
	return 1;
}

/* 只结束遍历；字体发现的结果体积很小，缓存到进程退出，避免每次打开视图都重新查询 */
void font_list_close(void)
{
	cur = nfonts;
}
