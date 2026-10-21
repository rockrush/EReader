#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "draw.h"

#define TX_MAX_SIZE	(32 << 20)	/* 文本文件读取上限 */
#define TX_MAX_LINES	(1 << 20)	/* 折行后的显示行上限 */
#define TX_TAB		4		/* tab 展开后的列数 */
#define TX_FONT_PX	5		/* 基准字号（像素），按屏幕缩放倍数放大 */
#define TX_PX		(TX_FONT_PX * FBM_FONT_SCALE)

struct tx_line {
	size_t off;	/* 在文本缓冲区中的偏移 */
	size_t len;	/* 字节长度（不含换行） */
};

struct tx_view {
	char *text;
	size_t size;
	struct tx_line *lines;	/* 折行后的显示行 */
	int n;
	int top;	/* 顶部显示行号 */
};

static uint16_t tx_w, tx_h;

static int tx_line_h(void)
{
	return MAX(1, fm_line_height(TX_PX)) + 2 * FBM_FONT_SCALE;
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
		struct tx_line *tmp = realloc(v->lines, (size_t) ncap * sizeof(*tmp));
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
	size_t pos = 0, last = 0;
	int w = 0;

	while (pos < len) {
		const char *p = s + pos;
		unsigned int cp = fm_utf8(&p);
		size_t n = (size_t) (p - (s + pos));
		int cw = fm_advance(cp, TX_PX);

		if (pos > 0 && w + cw > avail)
			return last ? last : pos;
		w += cw;
		if (cp == ' ')
			last = pos + n;
		pos += n;
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

/* 载入文本：展开 tab、过滤控制字符，保留合法的 UTF-8 序列 */
// TODO: 读不了 /proc 下大小为零的文件
static char *tx_load(const char *path, size_t *size)
{
	FILE *fp;
	char *in, *out;
	size_t len, n = 0, i, tabs = 0, col = 0;
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
			size_t stop = (col / TX_TAB + 1) * TX_TAB;
			while (col < stop) {
				out[n++] = ' ';
				col++;
			}
		} else if (c == '\r') {
			if (i + 1 < len && in[i + 1] == '\n')
				continue;	/* CRLF 只保留 LF */
			out[n++] = '\n';
			col = 0;
		} else if (c == '\n') {
			out[n++] = '\n';
			col = 0;
		} else if (c < 0x20 || c == 0x7f) {
			out[n++] = ' ';
			col++;
		} else if (c < 0x80) {
			out[n++] = (char) c;
			col++;
		} else {
			const char *p = in + i;
			unsigned int cp = fm_utf8(&p);
			size_t nb = (size_t) (p - (in + i));

			if (nb > 1) {
				memcpy(out + n, in + i, nb);
				n += nb;
				i += nb - 1;
				col += cp >= 0x1100 ? 2 : 1;	/* 宽字符按两列计 */
			} else {
				out[n++] = '?';	/* 非法 UTF-8 字节 */
				col++;
			}
		}
	}
	out[n] = 0;
	free(in);
	*size = n;
	return out;
}

/* 绘制 len 字节的文本（UTF-8），返回绘制后的横向位置 */
static int tx_text(int x, int y, const char *s, size_t len, unsigned r, unsigned g, unsigned b)
{
	return fm_text_n(x, y, s, len, TX_PX, r, g, b);
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

	tx_w = h;
	tx_h = w;
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
