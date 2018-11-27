
#ifndef _WEBSOCKET_COMMON_H_
#define _WEBSOCKET_COMMON_H_

#include <stdbool.h>

// #define WEBSOCKET_DEBUG

// websocket根据data[0]判别数据包类型    比如0x81 = 0x80 | 0x1 为一个txt类型数据包
typedef enum{
    WDT_MINDATA = -20,      // 0x0：标识一个中间数据包
    WDT_TXTDATA = -19,      // 0x1：标识一个txt类型数据包
    WDT_BINDATA = -18,      // 0x2：标识一个bin类型数据包
    WDT_DISCONN = -17,      // 0x8：标识一个断开连接类型数据包
    WDT_PING = -16,     // 0x8：标识一个断开连接类型数据包
    WDT_PONG = -15,     // 0xA：表示一个pong类型数据包
    WDT_ERR = -1,
    WDT_NULL = 0
}WebsocketData_Type;

int webSocket_clientLinkToServer(char *ip, int port, char *interface_path);
int webSocket_serverLinkToClient(int fd, char *recvBuf, int bufLen);

int webSocket_send(int fd, char *data, int dataLen, bool isMask, WebsocketData_Type type);
int webSocket_recv(int fd, char *data, int dataMaxLen, int *dataType);

void webSocket_delayms(unsigned int ms);

// 其它工具
int websocket_getIpByHostName(char *hostName, char *backIp);//域名转 IP
int netCheck_setIP(char *devName, char *ip);
void netCheck_getIP(char *devName, char *ip);//设置和获取本机IP  *devName设备名称如: eth0

#endif

