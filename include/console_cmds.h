#ifndef H_CONSOLE_CMDS
#define H_CONSOLE_CMDS

// 回复回调：由 monitor.c 提供，负责把文本写回控制台连接
typedef void (*cc_reply_t)(int fd, const char *text);

void cc_give     (int fd, cc_reply_t reply, const char *arg);
void cc_tp       (int fd, cc_reply_t reply, const char *arg);
void cc_tpall    (int fd, cc_reply_t reply, const char *arg);
void cc_heal     (int fd, cc_reply_t reply, const char *arg);
void cc_kill     (int fd, cc_reply_t reply, const char *arg);
void cc_clear    (int fd, cc_reply_t reply, const char *arg);
void cc_time     (int fd, cc_reply_t reply, const char *arg);
void cc_setblock (int fd, cc_reply_t reply, const char *arg);
void cc_fill     (int fd, cc_reply_t reply, const char *arg);
void cc_save     (int fd, cc_reply_t reply, const char *arg);
void cc_players  (int fd, cc_reply_t reply, const char *arg);
void cc_spawn    (int fd, cc_reply_t reply, const char *arg);

#endif
