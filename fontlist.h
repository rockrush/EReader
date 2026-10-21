#ifndef FONTLIST_H
#define FONTLIST_H

struct font_candidate {
	const char *path;
	int index;	/* 字体集合（.ttc/.otc）内的序号，单文件字体为 0 */
};

/* 按优先级准备系统字体候选：EREADER_FONT 指定的字体最先，其余由系统配置决定。
 * 结果会缓存，重复调用不会重新扫描系统。 */
int font_list_open(void);
/* 取下一种候选字体，返回 0 表示已到末尾 */
int font_list_next(struct font_candidate *fc);
/* 结束本轮遍历（候选表仍保留供下次使用） */
void font_list_close(void);

#endif
