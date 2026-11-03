#ifndef H_MONITOR
#define H_MONITOR

// 启动监控服务（独立 TCP 端口，连上即返回一行 JSON）
void monitor_start (void);

// 由主程序提供的运行时信息
int monitor_get_players (void);
int monitor_get_sockets (void);

#endif
