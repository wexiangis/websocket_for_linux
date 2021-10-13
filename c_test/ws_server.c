#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h> //非阻塞宏
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "ws_server.h"

#define WSS_ERR(fmt) fprintf(stderr, "[WSS_ERR] %s(%d): " fmt, __func__, __LINE__)
#define WSS_ERR2(fmt, argv...) fprintf(stderr, "[WSS_ERR] %s(%d): " fmt, __func__, __LINE__, ##argv)

//服务器副线程,负责检测 数据接收 和 客户端断开
static void server_thread2(void *argv);

//抛线程工具
static void new_thread(void *obj, void *callback)
{
    pthread_t th;
    pthread_attr_t attr;
    int ret;
    //禁用线程同步,线程运行结束后自动释放
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    //抛出线程
    ret = pthread_create(&th, &attr, callback, (void *)obj);
    if (ret != 0)
        WSS_ERR2("pthread_create failed !! %s\r\n", strerror(ret));
    //attr destroy
    pthread_attr_destroy(&attr);
}

//注意在"epoll_wait时"或"目标fd已close()"的情况下会ctrl会失败
static void _epoll_ctrl(int fd_epoll, int fd, uint32_t event, int ctrl, void *ptr)
{
    struct epoll_event ev;
    ev.events = event;
    if (ptr)
        ev.data.ptr = ptr;
    else
        ev.data.fd = fd;
    if (epoll_ctl(fd_epoll, ctrl, fd, &ev) != 0)
        WSS_ERR2("epoll ctrl %d error !!\r\n", ctrl);
}

/*
 *  接收数据,自动连接客户端
 *  返回: 
 *      >0 有数据
 *      =0 无数据
 *      -1 连接异常
 */
static int client_recv(Ws_Client *wsc)
{
    int ret;
    char buff[WS_SERVER_PKG] = {0};
    Ws_DataType retPkgType = WDT_NULL;

    ret = ws_recv(wsc->fd, buff, sizeof(buff), &retPkgType);

    //这可能是一包客户端请求
    if (!wsc->isLogin && ret < 0 &&
        strncmp(buff, "GET", 3) == 0 &&
        strstr(buff, "Sec-WebSocket-Key"))
    {
        //构建回复
        if (ws_responseClient(wsc->fd, buff, -ret, wsc->wss->path) > 0)
        {
            //这个延时很有必要,否则下面onLogin里面发东西客户端可能收不到
            ws_delayms(5);
            wsc->isLogin = true;
            //回调
            if (wsc->wss->onLogin)
                wsc->wss->onLogin(wsc);
            return 0;
        }
        //websocket握手失败,标记断开类型
        else
        {
            wsc->exitType = WET_LOGIN;
            return -1;
        }
    }
    //接收数据量统计
    if (ret != 0)
        wsc->recvBytes += ret > 0 ? ret : (-ret);
    //消息回调
    if (wsc->wss->onMessage)
        wsc->wss->onMessage(wsc, buff, ret, retPkgType);
    //断连协议,标记断开类型
    if (retPkgType == WDT_DISCONN)
    {
        wsc->exitType = WET_PKG_DIS;
        return -1;
    }
    //正常返回
    return ret == 0 ? 0 : 1;
}

//onMessage异步回调
// static void client_onMessage(void *argv)
// {
//     Ws_Client *wsc = (Ws_Client *)argv;
//     int ret = 1;
//     //收完为止
//     while (ret > 0)
//         ret = client_recv(wsc);
// }

//onExit异步回调
static void client_onExit(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    if (wsc->wss->onExit)
        wsc->wss->onExit(wsc, wsc->exitType);
    //重置结构体,给下次使用
    memset(wsc, 0, sizeof(Ws_Client));
}

//取得空闲的坑
static Ws_Client *client_get(Ws_Server *wss, int fd)
{
    int i;
    for (i = 0; i < WS_SERVER_CLIENT; i++)
    {
        if (!wss->client[i].fd &&
            !wss->client[i].isExiting &&
            !wss->client[i].wst)
        {
            memset(&wss->client[i], 0, sizeof(Ws_Client));
            wss->client[i].fd = fd; //占用
            wss->client[i].wss = wss;
            wss->client[i].priv = wss->priv;
            return &wss->client[i];
        }
    }
    WSS_ERR2("failed, out of range(%d) !!\r\n", WS_SERVER_CLIENT); //满员
    return NULL;
}

//共用代码块,完成客户端加人、客户端结构初始化、注册epoll监听
#define COMMON_CODE() \
wss->clientCount += 1;\
wst[i].clientCount += 1;\
wsc->fd = fd;\
wsc->wss = wss;\
wsc->wst = &wss->thread[i];\
wsc->order = wss->clientCount;\
_epoll_ctrl(wst[i].fd_epoll, wsc->fd, EPOLLIN, EPOLL_CTL_ADD, wsc);

//添加客户端
static void client_add(Ws_Server *wss, int fd)
{
    int i;
    Ws_Thread *wst = wss->thread;
    //取得空闲客户端指针
    Ws_Client *wsc = client_get(wss, fd);
    if (!wsc)
        return;
    //遍历线程,谁有空谁托管
    for (i = 0; i < WS_SERVER_THREAD; i++)
    {
        if (wst[i].clientCount < WS_SERVER_CLIENT_OF_THREAD && //线程未满员
            wst[i].isRun && //线程在运行
            wst[i].fd_epoll) //线程epoll正常
        {
            //共用代码块
            COMMON_CODE();
            return;
        }
    }
    //开启新线程
    for (i = 0; i < WS_SERVER_THREAD; i++)
    {
        if (!wst[i].isRun &&
            wst[i].fd_epoll == 0 &&
            wst[i].clientCount == 0)
        {
            //参数初始化
            wst[i].wss = wss;
            wst[i].fd_epoll = epoll_create(WS_SERVER_CLIENT_OF_THREAD);
            //开线程
            new_thread(&wst[i], &server_thread2);
            //共用代码块
            COMMON_CODE();
            return;
        }
    }
    WSS_ERR2("failed, out of range(%d) !!\r\n", WS_SERVER_CLIENT); //线程负荷已满
    memset(wsc, 0, sizeof(Ws_Client)); //释放占用的坑
}

//移除特定客户端
static void client_del(Ws_Thread *wst, Ws_Client *wsc)
{
    if (wsc->isExiting)
        return;
    //标记,防止反复del
    wsc->isExiting = true;
    //从epoll监听列表中移除
    _epoll_ctrl(wst->fd_epoll, wsc->fd, 0, EPOLL_CTL_DEL, wsc);
    //关闭描述符
    close(wsc->fd);
    //如有需则断连回调
    new_thread(wsc, &client_onExit);
    //减人
    wst->clientCount -= 1;
    wst->wss->clientCount -= 1;
}

//副线程检测异常客户端并移除
static void client_detect(Ws_Thread *wst, bool delAll)
{
    Ws_Client *client = wst->wss->client;
    int i;
    for (i = 0; i < WS_SERVER_CLIENT; i++)
    {
        if (client[i].wst == wst && //客户端属于该线程管辖
            client[i].fd && //这是有效连接
            !client[i].isExiting) //不是正在退出状态
        {
            //有异常错误 || 就是要删除
            if(client[i].exitType || delAll)
                client_del(wst, &client[i]);
            //非登录状态,进行websocket握手超时计数
            else if (!client[i].isLogin)
            {
                //5秒超时(延时不准,只是大概)
                client[i].loginTimeout += 500;
                if (client[i].loginTimeout > WS_SERVER_LOGIN_TIMEOUT_MS)
                {
                    client[i].exitType = WET_LOGIN_TIMEOUT;
                    client_del(wst, &client[i]);
                }
            }
        }
    }
}

//服务器副线程,负责检测 数据接收 和 客户端断开
//只要还有一个客户端在维护就不会退出线程
static void server_thread2(void *argv)
{
    Ws_Thread *wst = (Ws_Thread *)argv;
    int nfds, count;
    struct epoll_event events[WS_SERVER_CLIENT_OF_THREAD];

    while (!wst->wss->isExit && wst->clientCount > 0)
    {
        wst->isRun = true;
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时ms
        if ((nfds = epoll_wait(wst->fd_epoll, events, WS_SERVER_CLIENT_OF_THREAD, 500)) < 0)
        {
            WSS_ERR("epoll_wait failed\r\n");
            break;
        }
        for (count = 0; count < nfds; count++)
        {
            //epoll错误
            if ((events[count].events & EPOLLERR) || (events[count].events & EPOLLHUP))
            {
                //标记异常类型
                ((Ws_Client *)events[count].data.ptr)->exitType = WET_EPOLL;
                //移除
                client_del(wst, (Ws_Client *)events[count].data.ptr);
            }
            //接收数据事件
            else if (events[count].events & EPOLLIN)
                client_recv((Ws_Client *)events[count].data.ptr);
        }
        //异常客户端检查
        client_detect(wst, false);
    }
    wst->isRun = false;
    //关闭epoll描述符
    close(wst->fd_epoll);
    //关闭线程维护的所有客户端(正常情况应该都已经关闭了)
    client_detect(wst, true);
    //清空内存,下次使用
    memset(wst, 0, sizeof(Ws_Thread));
}

//服务器主线程,负责检测 新客户端接入
static void server_thread(void *argv)
{
    Ws_Server *wss = (Ws_Server *)argv;
    int ret, count;
    int fd_accept;

    socklen_t socAddrLen;
    struct sockaddr_in acceptAddr;
    struct sockaddr_in serverAddr = {0};

    int nfds;
    struct epoll_event events[WS_SERVER_CLIENT];

    serverAddr.sin_family = AF_INET; //设置为IP通信
    serverAddr.sin_addr.s_addr = INADDR_ANY; //服务器IP地址
    serverAddr.sin_port = htons(wss->port); //服务器端口号
    socAddrLen = sizeof(struct sockaddr_in);

    //socket init
    if ((wss->fd = socket(AF_INET, SOCK_STREAM, 0)) <= 0)
    {
        WSS_ERR("create socket failed\r\n");
        return;
    }

    //地址可重用设置(有效避免bind超时)
    setsockopt(wss->fd, SOL_SOCKET, SO_REUSEADDR, &ret, sizeof(ret));

    //设置为非阻塞接收
    ret = fcntl(wss->fd, F_GETFL, 0);
    fcntl(wss->fd, F_SETFL, ret | O_NONBLOCK);

    //bind
    count = 0;
    while (bind(wss->fd, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) != 0)
    {
        if (++count > WS_SERVER_BIND_TIMEOUT_MS)
        {
            WSS_ERR2("bind timeout %d 服务器端口占用中,请稍候再试\r\n", count);
            goto server_exit;
        }
        ws_delayms(1);
    }

    //listen
    if (listen(wss->fd, 0) != 0)
    {
        WSS_ERR2("listen failed\r\n");
        goto server_exit;
    }

    //创建一个epoll描述符
    wss->fd_epoll = epoll_create(WS_SERVER_CLIENT);

    //向epoll注册server_sockfd监听事件
    _epoll_ctrl(wss->fd_epoll, wss->fd, EPOLLIN | EPOLLET, EPOLL_CTL_ADD, NULL);

    while (!wss->isExit)
    {
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时ms
        if ((nfds = epoll_wait(wss->fd_epoll, events, WS_SERVER_CLIENT, 500)) < 0)
        {
            WSS_ERR("epoll_wait failed\r\n");
            break;
        }
        for (count = 0; count < nfds; count++)
        {
            //新通道接入事件
            if (events[count].data.fd == wss->fd)
            {
                fd_accept = accept(wss->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);
                //添加客户端
                if (fd_accept >= 0)
                    client_add(wss, fd_accept);
            }
        }
    }
    //移除所有副线程
    wss->isExit = true;
    //关闭epoll描述符
    close(wss->fd_epoll);
    wss->fd_epoll = 0;
server_exit:
    wss->isExit = true;
    //关闭socket
    close(wss->fd);
    wss->fd = 0;
}

void ws_server_release(Ws_Server **wss)
{
    if (wss)
    {
        if (*wss)
        {
            (*wss)->isExit = true;
            while ((*wss)->fd)
                ws_delayms(5);
            free(*wss);
            *wss = NULL;
        }
    }
}

Ws_Server* ws_server_create(
    int port,
    const char *path,
    void *priv,
    WsOnLogin onLogin,
    WsOnMessage onMessage,
    WsOnExit onExit)
{
    Ws_Server *wss = (Ws_Server*)calloc(1, sizeof(Ws_Server));
    wss->port = port;
    strcpy(wss->path, path ? path : "/");
    wss->priv = priv;
    wss->onLogin = onLogin;
    wss->onMessage = onMessage;
    wss->onExit = onExit;
    new_thread(wss, &server_thread);
    return wss;
}
