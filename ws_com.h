
#ifndef _WS_COM_H_
#define _WS_COM_H_

#include <stdbool.h>

// #define WS_DEBUG

// websocket根据data[0]判别数据包类型
// 比如0x81 = 0x80 | 0x1 为一个txt类型数据包
typedef enum
{
    WDT_NULL = 0, // 非标准数据包
    WDT_MINDATA,  // 0x0：中间数据包
    WDT_TXTDATA,  // 0x1：txt类型数据包
    WDT_BINDATA,  // 0x2：bin类型数据包
    WDT_DISCONN,  // 0x8：断开连接类型数据包 ws_recv 函数内自动 close
    WDT_PING,     // 0x8：ping类型数据包 ws_recv 函数内自动处理
    WDT_PONG,     // 0xA：pong类型数据包 ws_recv 函数内自动处理
} WsData_Type;

int ws_connectToServer(char *ip, int port, char *path, int timeout);
int ws_responseClient(int fd, char *data, int dataLen, char *path);

int ws_send(int fd, char *data, int dataLen, bool mask, WsData_Type type);
int ws_recv(int fd, char *data, int dataMaxLen, WsData_Type *type);

void ws_delayms(int ms);
char *ws_time(void); //返回时间戳,格式如"20:45:30"

//域名转IP工具,成功返回大于0请求时长ms,失败返回负值的请求时长ms
int ws_getIpByHostName(char *hostName, char *backIp);

#endif
