#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "draw.h"

#define TX_MAX_SIZE	(32 << 20)	/* 文本文件读取上限 */
#define TX_MAX_LINES	(1 << 20)	/* 折行后的显示行上限 */
#define TX_TAB		8		/* tab 展开后的列数 */
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

struct tx_line {
	size_t off;		/* 在文本缓冲区中的偏移 */
	size_t len;		/* 字节长度（不含换行） */
};

struct tx_view {
	char *text;
	size_t size;
	struct tx_line *lines;	/* 折行后的显示行 */
	int n;
	int top;		/* 顶部显示行号 */
};

static uint16_t tx_w, tx_h;

int fm_font_acquire(void);
int fm_font_release(void);
int fm_advance(unsigned char c);
int fm_char(int x, int y, unsigned char c, unsigned r, unsigned g, unsigned b);

extern int fm_line_height;
static int tx_line_h(void)
{
	return MAX(1, fm_line_height) + 2 * FBM_FONT_SCALE;
}

static int tx_visible(void)
{
	int visible = (tx_h - 12 * FBM_FONT_SCALE) / tx_line_h();
	return MAX(1, visible);
}

static int tx_push(struct tx_view *v, int *cap, size_t off, size_t len)
{
	if (v->n == *cap) {
		int ncap = *cap ? *cap * 2 : 256;
		struct tx_line *tmp = realloc(v->lines,
			(size_t) ncap * sizeof(*tmp));

		if (!tmp)
			return 1;
		v->lines = tmp;
		*cap = ncap;
	}
	v->lines[v->n].off = off;
	v->lines[v->n].len = len;
	v->n++;
	return 0;
}

/* 返回 s 前 len 字节中能放进 avail 像素宽度的字节数（优先在空格后断开） */
static size_t tx_fit(const char *s, size_t len, int avail)
{
	size_t i, last = 0;
	int w = 0;

	for (i = 0; i < len; i++) {
		int cw = fm_advance((unsigned char) s[i]);
		if (i > 0 && w + cw > avail)
			return last ? last : i;
		w += cw;
		if (s[i] == ' ')
			last = i + 1;
	}
	return len;
}

/* 把文本按可用宽度切成显示行 */
static int tx_format(struct tx_view *v, int avail)
{
	size_t pos = 0;
	int cap = 0;

	if (avail < 1)
		avail = 1;
	while (pos <= v->size && v->n < TX_MAX_LINES) {
		size_t end = pos;

		while (end < v->size && v->text[end] != '\n')
			end++;
		if (end == pos && tx_push(v, &cap, pos, 0))
			return 1;
		while (pos < end && v->n < TX_MAX_LINES) {
			size_t len = tx_fit(v->text + pos, end - pos, avail);

			if (len < 1)
				len = 1;
			if (tx_push(v, &cap, pos, len))
				return 1;
			pos += len;
		}
		if (end >= v->size)
			break;
		pos = end + 1;
	}
	return 0;
}

/* 载入文本：展开 tab，UTF-8 与控制字符换成可显示的单字节 */
// TODO: 读不了 /proc 下大小为零的文件
static char *tx_load(const char *path, size_t *size)
{
	FILE *fp;
	char *in, *out;
	size_t len, n = 0, i, tabs = 0;
	long sz;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return NULL;
	}
	len = (size_t) sz > TX_MAX_SIZE ? TX_MAX_SIZE : (size_t) sz;
	if ((size_t) sz > len)
		fprintf(stderr, "%s: only the first %d MB are shown\n", path,
			TX_MAX_SIZE >> 20);
	in = malloc(len + 1);
	if (!in) {
		fclose(fp);
		return NULL;
	}
	len = fread(in, 1, len, fp);
	fclose(fp);
	in[len] = 0;

	for (i = 0; i < len; i++)
		if (in[i] == '\t')
			tabs++;
	out = malloc(len + tabs * (TX_TAB - 1) + 1);
	if (!out) {
		free(in);
		return NULL;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) in[i];
		if (c == '\t') {
			size_t stop = (n / TX_TAB + 1) * TX_TAB;
			while (n < stop)
				out[n++] = ' ';
		} else if (c == '\r') {
			if (i + 1 < len && in[i + 1] == '\n')
				continue;	/* CRLF 只保留 LF */
			out[n++] = '\n';
		} else if (c >= 0x80) {
			while (i + 1 < len && ((unsigned char) in[i + 1] & 0xc0) == 0x80)
				i++;
			out[n++] = '?';
		} else if (c < 0x20 && c != '\n') {
			out[n++] = ' ';
		} else if (c == 0x7f) {
			out[n++] = ' ';
		} else {
			out[n++] = (char) c;
		}
	}
	out[n] = 0;
	free(in);
	*size = n;
	return out;
}

/* 绘制 len 字节的文本（调用前已清理为 ASCII），返回绘制后的横向位置 */
static int tx_text(int x, int y, const char *s, size_t len, unsigned r, unsigned g, unsigned b)
{
	for (size_t i = 0; i < len; i++)
		x += fm_char(x, y, (unsigned char) s[i], r, g, b);
	return x;
}

static void tx_draw(struct tx_view *v)
{
	int pad = 6 * FBM_FONT_SCALE;
	int line_h = tx_line_h();
	int visible = tx_visible();

	fill_rect(0, 0, tx_h, tx_w, 255, 255, 255);
	for (int i = 0; i < visible && v->top + i < v->n; i++) {
		const struct tx_line *ln = &v->lines[v->top + i];
		tx_text(pad, pad + i * line_h, v->text + ln->off, ln->len, UI_ICON, UI_ICON, UI_ICON);
	}
}

/* 文本阅读器：j/k 上下翻半屏，q 退出 */
int show_text(const char *path, uint16_t w, uint16_t h)
{
	struct tx_view v;
	int c, step, visible, maxtop, retval = EXIT_FAILURE;

	tx_w = w;
	tx_h = h;
	memset(&v, 0, sizeof(v));
	if (fm_font_acquire())
		return EXIT_FAILURE;
	v.text = tx_load(path, &v.size);
	if (!v.text) {
		fprintf(stderr, "cannot open <%s>\n", path);
		goto out;
	}
	if (tx_format(&v, tx_w - 12 * FBM_FONT_SCALE))
		goto out;
	visible = tx_visible();
	maxtop = MAX(0, v.n - visible);
	tx_draw(&v);
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':	// 退出
			retval = EXIT_SUCCESS;
			goto out;
		case 'j':	// 向下翻半屏
			step = MAX(1, visible / 2);
			v.top = MIN(maxtop, v.top + step);
			break;
		case 'k':	// 向上翻半屏
			step = MAX(1, visible / 2);
			v.top = MAX(0, v.top - step);
			break;
		default:
			continue;
		}
		tx_draw(&v);
	}
	retval = EXIT_SUCCESS;
out:
	free(v.lines);
	free(v.text);
	fm_font_release();
	return retval;
}
