#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws_server.h"

/*
 *  接收数据回调
 *  参数:
 *      wsc: 客户端信息结构体指针
 *      msg: 接收数据内容
 *      msgLen: >0时为websocket数据包,<0时为非包数据,没有=0的情况
 *      type： websocket包类型
 */
void onMessage(Ws_Client *wsc, char *msg, int msgLen, Ws_DataType type)
{
    int ret = 0;
    //正常 websocket 数据包
    if (msgLen > 0)
    {
        //客户端过多时不再打印,避免卡顿
        if (wsc->wss->clientCount <= WS_SERVER_CLIENT_OF_PRINTF)
            printf("onMessage: fd/%03d order/%03d total/%03d %d/%dbytes %s\r\n",
                wsc->fd, wsc->order, wsc->wss->clientCount, msgLen, wsc->recvBytes, msgLen < 128 ? msg : " ");

        //在这里根据客户端的请求内容, 提供相应的回复
        if (strstr(msg, "Say hi~") != NULL)
            ;
        //回显,收到什么回复什么
        else
            ret = ws_send(wsc->fd, msg, msgLen, false, type);
        //发送失败,标记异常(后续会被自动回收)
        if (ret < 0)
            wsc->exitType = WET_SEND;
    }
    //非 websocket 数据包
    else if (msgLen < 0)
    {
        msgLen = -msgLen;
        printf("onMessage: fd/%03d order/%03d total/%03d %d/%dbytes bad pkg %s\r\n",
               wsc->fd, wsc->order, wsc->wss->clientCount, msgLen, wsc->recvBytes, msgLen < 128 ? msg : " ");
    }
    //特殊包(不需作任何处理,知道就行)
    else
    {
        if (type == WDT_PING)
            printf("onMessage: fd/%03d order/%03d total/%03d pkg WDT_PING \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
        else if (type == WDT_PONG)
            printf("onMessage: fd/%03d order/%03d total/%03d pkg WDT_PONG \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
        else if (type == WDT_DISCONN)
            printf("onMessage: fd/%03d order/%03d total/%03d pkg WDT_DISCONN \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    }
}

//客户端接入时(已连上),你要做什么?
void onLogin(Ws_Client *wsc)
{
    printf("onLogin: fd/%03d order/%03d total/%03d\r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    //打招呼
    ws_send(wsc->fd, "Say hi~ I am server", 19, false, WDT_TXTDATA);
}

//客户端断开时(已断开),你要做什么?
void onExit(Ws_Client *wsc, Ws_ExitType exitType)
{
    //断开原因
    if (exitType == WET_EPOLL)
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by epoll\r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else if (exitType == WET_SEND)
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by send\r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else if (exitType == WET_LOGIN)
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by login failed \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else if (exitType == WET_LOGIN_TIMEOUT)
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by login timeout \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else if (exitType == WET_PKG_DIS)
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by pkg dis \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else
        printf("onExit: fd/%03d order/%03d total/%03d disconnect by unknow \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
}

/*
 *  usage: ./server port path
 */
int main(int argc, char **argv)
{
    int i;
    char buff[1024];

    //服务器必须参数
    Ws_Server *wss = ws_server_create(
        argc > 1 ? atoi(argv[1]) : 9999, //服务器端口
        argc > 2 ? argv[2] : "/", //服务器路径(这样写表示路径为空)
        NULL, //指向自己的数据的指针,回调函数里使用 wsc->priv 取回
        &onLogin, //客户端接入时(已连上),你要做什么?
        &onMessage, //收到客户端数据时,你要做什么?
        &onExit); //客户端断开时(已断开),你要做什么?

    //服务器启动至少先等3秒(有时会bind超时)
    while (!wss->isExit)
    {
        ws_delayms(3000);
        //每3秒推送信息给所有客户端
        for (i = 0; i < WS_SERVER_CLIENT; i++)
        {
            if (wss->client[i].fd && wss->client[i].isLogin && !wss->client[i].exitType)
            {
                snprintf(buff, sizeof(buff), "Tips from server fd/%03d order/%03d total/%03d %s",
                         wss->client[i].fd, wss->client[i].order, wss->clientCount, ws_time());

                // if (ws_send(wss->client[i].fd, buff, sizeof(buff), false, WDT_TXTDATA) < 0) //大数据量压力测试
                if (ws_send(wss->client[i].fd, buff, strlen(buff), false, WDT_TXTDATA) < 0)
                {
                    //发送失败,标记异常
                    wss->client[i].exitType = WET_SEND;
                }
            }
        }
    }

    ws_server_release(&wss);
    printf("server exit \r\n");
    return 0;
}
