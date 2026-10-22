#include <stddef.h>

#define TIME_PERIOD	2	// 更新时间的间隔
#define FBM_FONT_SCALE	3
#define UI_BG		235	// 按钮/纸张填充色
#define UI_BORDER	90	// 按钮/电池边框色
#define UI_ICON		45	// 图标/文字颜色
#define	UI_SHADOW	128	// 阴影颜色

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

int fb_init(char *dev);
void fb_free(void);
void *fb_mem(int r);
void fb_cmap(void);
unsigned fb_val(int r, int g, int b);
void fb_get(char *d, unsigned *r, unsigned *g, unsigned *b);
void fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b);
int readkey(void);
