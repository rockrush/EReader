#ifndef _EREADER_H
#define _EREADER_H
#include <stdint.h>
#include <termios.h>
#include <limits.h>

#define GRAY	128	// 屏幕空白处的灰色填充

struct ereader_cfg {
	uint16_t scr_w;
	uint16_t scr_h;
	uint8_t ui_h;
	int32_t rotate;
	struct termios term;
	char path[PATH_MAX];	// 当前处理路径
};

extern struct ereader_cfg cfg;

void px(int x, int y, unsigned r, unsigned g, unsigned b);

#endif
