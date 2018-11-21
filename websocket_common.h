
#ifndef _WEBSOCKET_COMMON_H_
#define _WEBSOCKET_COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>     // 使用 malloc, calloc等动态分配内存方法
#include <stdbool.h>
#include <time.h>       // 获取系统时间
#include <errno.h>
#include <fcntl.h>      // 非阻塞
#include <sys/un.h> 
#include <arpa/inet.h>  // inet_addr()
//文件IO操作
#include <unistd.h>     // close()
#include <sys/types.h>
#include <sys/socket.h>
//域名转IP
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>      // gethostbyname, gethostbyname2, gethostbyname_r, gethostbyname_r2
#include <sys/un.h> 
#include <sys/epoll.h>  // epoll管理服务器的连接和接收触发
#include <pthread.h>    // 使用多线程

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

int webSocket_send(int fd, char *data, int dataLen, bool mod, WebsocketData_Type type);
int webSocket_recv(int fd, char *data, int dataMaxLen);

void webSocket_delayms(unsigned int ms);

//
int websocket_getIpByHostName(char *hostName, char *backIp);

#endif

