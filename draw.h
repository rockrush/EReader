#include <stddef.h>

/* fb_mode() interpretation */
#define FBM_BPP(m)	(((m) >> 16) & 0x0f)	/* bytes per pixel (4 bits) */
#define FBM_CLR(m)	((m) & 0x0fff)		/* bits per color (12 bits) */
#define FBM_ORD(m)	(((m) >> 20) & 0x07)	/* color order (3 bits) */
#define	FBM_FONT_SCALE	3
#define UI_BG		235	/* 按钮/纸张填充色 */
#define UI_BORDER	90	/* 按钮/电池边框色 */
#define UI_ICON		45	/* 图标/文字颜色 */

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

int fb_init(char *dev);
void fb_free(void);
unsigned fb_mode(void);
void *fb_mem(int r);
int fb_rows(void);
int fb_cols(void);
void fb_cmap(void);
unsigned fb_val(int r, int g, int b);
void fb_get(char *d, unsigned *r, unsigned *g, unsigned *b);
void fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b);
int readkey(void);

/* UTF-8 文本绘制（filemgr.c），px 为像素字号 */
int fm_font_acquire(void);
void fm_font_release(void);
int fm_line_height(int px);
unsigned int fm_utf8(const char **s);
int fm_advance(unsigned int cp, int px);
int fm_char(int x, int y, unsigned int cp, int px, unsigned r, unsigned g, unsigned b);
int fm_text_n(int x, int y, const char *s, size_t len, int px, unsigned r, unsigned g, unsigned b);
int fm_text(int x, int y, const char *s, int px, unsigned r, unsigned g, unsigned b);
