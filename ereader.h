#ifndef _EREADER_H
#define _EREADER_H
#include <stdint.h>
#include <limits.h>

struct ereader_cfg {
	union {
		uint16_t scr_w;	// 屏幕宽度
		uint16_t view_h;// 正文区高度
		uint16_t ui_h;	// UI区高度
	};
	uint16_t scr_h;
	uint16_t view_w;	// 正文区宽度
	uint16_t ui_w;		// UI区宽度
	uint8_t running;	// 继续运行
	char path[PATH_MAX];	// 当前处理路径
};

extern struct ereader_cfg cfg;

void px(int x, int y, unsigned r, unsigned g, unsigned b);
void fb_set(char *d, unsigned r, unsigned g, unsigned b);

#endif
