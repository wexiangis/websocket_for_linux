
#ifndef _WS_COM_H_
#define _WS_COM_H_

#include <stdbool.h>

// #define WS_DEBUG

// websocket根据data[0]判别数据包类型
// 比如0x81 = 0x80 | 0x1 为一个txt类型数据包
typedef enum
{
    WDT_MINDATA = -20, // 0x0：标识一个中间数据包
    WDT_TXTDATA = -19, // 0x1：标识一个txt类型数据包
    WDT_BINDATA = -18, // 0x2：标识一个bin类型数据包
    WDT_DISCONN = -17, // 0x8：标识一个断开连接类型数据包
    WDT_PING = -16,    // 0x8：标识一个断开连接类型数据包
    WDT_PONG = -15,    // 0xA：表示一个pong类型数据包
    WDT_ERR = -1,
    WDT_NULL = 0
} WsData_Type;

int ws_connectToServer(char *ip, int port, char *path, int timeout);
int ws_responseClient(int fd, char *recvBuf, int bufLen);

int ws_send(int fd, char *data, int dataLen, bool mask, WsData_Type type);
int ws_recv(int fd, char *data, int dataMaxLen, WsData_Type *type);

void ws_delayms(int ms);

//域名转IP工具,成功返回大于0请求时长ms,失败返回负值的请求时长ms
int ws_getIpByHostName(char *hostName, char *backIp);

#endif
