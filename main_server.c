
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

#include "ws_com.h"

// ================== 服务器内部功能实现 ===================

//发包数据量 10K
#define SEND_PKG_MAX (10240)

//收包缓冲区大小 10K+
#define RECV_PKG_MAX (SEND_PKG_MAX + 16)

//限制最大可接入客户端数量
//普通计算机接入破千之后CPU会逐渐拉满,无法再接入/发起更多客户端(性能因素)
#define CLIENT_MAX 1000

//bind超时ms(通常为服务器端口被占用,或者有客户端还连着上次的服务器)
#define BIND_TIMEOUTMS 1000

//客户端事件回调函数原型
typedef void (*OnLogin)(void *obj);
typedef void (*OnExit)(void *obj, int exitType);
typedef void (*OnMessage)(void *obj, char *msg, int msgLen, WsData_Type type);

//客户端线程使用的参数结构体
typedef struct WsClient
{
    int fd; //accept之后得到的客户端连接控制符
    int *fdTotal; //当前接入客户端总数
    char *path; //服务器路径
    bool login; //是否完成websocket登录验证
    bool exit; //线程结束标志
    bool err; //异常标志
    unsigned int recvBytes; //总接收字节计数
    void *privateData; //用指针传递自己的数据到回调函数里使用,而不是全局变量
    OnLogin onLogin;
    OnExit onExit;
    OnMessage onMessage;
} Ws_Client;

//服务器线程使用的参数结构体
typedef struct WsServer
{
    int fd; //服务器控制符
    int fd_epoll; //epoll控制符
    int fdTotal; //当前接入客户端总数
    int port; //服务器端口
    char path[128]; //服务器路径
    bool exit; //线程结束标志
    void *privateData; //用指针传递自己的数据到回调函数里使用,而不是全局变量
    Ws_Client *client[CLIENT_MAX]; //当前接入客户端列表
    OnLogin onLogin;
    OnExit onExit;
    OnMessage onMessage;
} Ws_Server;

//抛线程工具
void new_thread(void *obj, void *callback)
{
    pthread_t th;
    pthread_attr_t attr;
    //禁用线程同步,线程运行结束后自动释放
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    //抛出线程
    pthread_create(&th, &attr, callback, (void *)obj);
    //attr destroy
    pthread_attr_destroy(&attr);
}

//注意在"epoll_wait时"或"目标fd已close()"的情况下会ctrl会失败
void _epoll_ctrl(int fd_epoll, int fd, uint32_t event, int ctrl)
{
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    if (epoll_ctl(fd_epoll, ctrl, fd, &ev) != 0)
        printf("epoll ctrl %d error !!\r\n", ctrl);
}

/*
 *  接收数据,自动连接客户端
 *  返回: 
 *      >0 有数据
 *      =0 无数据
 *      -1 登录失败
 *      -2 WDT_DISCONN包
 */
int client_recv(Ws_Client *wsc)
{
    int ret;
    char buff[RECV_PKG_MAX] = {0};
    WsData_Type retPkgType = WDT_NULL;

    ret = ws_recv(wsc->fd, buff, sizeof(buff), &retPkgType);

    //这可能是一包客户端请求
    if (!wsc->login && ret < 0 &&
        strncmp(buff, "GET", 3) == 0 &&
        strstr(buff, "Sec-WebSocket-Key"))
    {
        //构建回复
        ret = ws_responseClient(wsc->fd, buff, ret, wsc->path);
        //连接成功,标记登录
        if (ret > 0)
        {
            //这个延时很有必要,否则下面onLogin里面发东西客户端可能收不到
            ws_delayms(5);
            wsc->login = true;
            //回调
            if (wsc->onLogin)
                wsc->onLogin(wsc);
            return 0;
        }
        //发送失败,连接断开
        else
            return -1;
    }
    //接收数据量统计
    if (wsc->login && ret != 0)
        wsc->recvBytes += ret > 0 ? ret : (-ret);
    //收到特殊包(由于这些比较底层,所以没有放到onMessage事件)
    if (retPkgType == WDT_DISCONN)
    {
        printf("specialPkg: fd/%03d/%03d recv WDT_DISCONN \r\n", wsc->fd, *wsc->fdTotal);
        return -2;
    }
    else if (retPkgType == WDT_PING)
        printf("specialPkg: fd/%03d/%03d recv WDT_PING \r\n", wsc->fd, *wsc->fdTotal);
    else if (retPkgType == WDT_PONG)
        printf("specialPkg: fd/%03d/%03d recv WDT_PONG \r\n", wsc->fd, *wsc->fdTotal);
    //回调
    else if (ret != 0 && wsc->onMessage)
        wsc->onMessage(wsc, buff, ret, retPkgType);
    //正常返回
    return ret == 0 ? 0 : 1;
}

//客户端维护线程,负责 登录处理 和 数据接收
void client_thread(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    int ret;
    unsigned int intervalMs = 10;
    unsigned int loginTimeout = 0; //等待登录超时
    int exitType = 0; //客户端断开原因

    while (!wsc->exit && !wsc->err)
    {
        //周期接收,每次收完为止
        do {
            ret = client_recv(wsc);
        } while (ret > 0);
        //收包异常
        if (ret < 0)
        {
            exitType = ret == (-1) ? 1 : 2;
            break;
        }
        //连接后半天不登录,踢出客户端
        if (!wsc->login)
        {
            loginTimeout += intervalMs;
            if (loginTimeout > 5000)
            {
                exitType = 3;
                break;
            }
        }
        ws_delayms(intervalMs);
    }
    //标记异常,请求移除
    wsc->err = true;
    //回调
    if (wsc->onExit)
        wsc->onExit(wsc, exitType);
    //等待被server_thread移除
    while (!wsc->exit)
        ws_delayms(50);
    //关闭控制符
    close(wsc->fd);
    //释放内存
    free(wsc);
}

//创建单独的客户端维护线程
void client_create(Ws_Server *wss, int fd)
{
    int i;
    for (i = 0; i < CLIENT_MAX; i++)
    {
        if (!wss->client[i])
        {
            wss->fdTotal += 1;
            wss->client[i] = (Ws_Client *)calloc(1, sizeof(Ws_Client));
            wss->client[i]->fd = fd;
            wss->client[i]->fdTotal = &wss->fdTotal;
            wss->client[i]->path = wss->path;
            wss->client[i]->privateData = wss->privateData;
            wss->client[i]->onLogin = wss->onLogin;
            wss->client[i]->onExit = wss->onExit;
            wss->client[i]->onMessage = wss->onMessage;
            new_thread(wss->client[i], &client_thread);
            return;
        }
    }
}

//移除客户端,fd=-1时移除所有
void client_remove(Ws_Server *wss, int fd)
{
    int i;
    for (i = 0; i < CLIENT_MAX; i++)
    {
        if (wss->client[i] && (wss->client[i]->fd == fd || fd < 0))
        {
            wss->fdTotal -= 1;
            wss->client[i]->exit = true; //通知客户端线程结束连接
            wss->client[i] = NULL;       //解除占用(内存在客户端线程中释放)
        }
    }
}

//检测异常客户端并移除
void client_check(Ws_Server *wss)
{
    int i;
    for (i = 0; i < CLIENT_MAX; i++)
    {
        if (wss->client[i] && wss->client[i]->err)
        {
            //从epoll监听列表中移除控制符
            _epoll_ctrl(wss->fd_epoll, wss->client[i]->fd, 0, EPOLL_CTL_DEL);
            //移除客户端
            client_remove(wss, wss->client[i]->fd);
        }
    }
}

//服务器主线程,负责检测 新客户端接入 及 客户端断开
void server_thread(void *argv)
{
    Ws_Server *wss = (Ws_Server *)argv;
    int ret, count;
    int fd_accept;

    socklen_t socAddrLen;
    struct sockaddr_in acceptAddr;
    struct sockaddr_in serverAddr = {0};

    int nfds;
    struct epoll_event events[CLIENT_MAX];

    serverAddr.sin_family = AF_INET;         //设置为IP通信
    serverAddr.sin_addr.s_addr = INADDR_ANY; //服务器IP地址
    serverAddr.sin_port = htons(wss->port);  //服务器端口号
    socAddrLen = sizeof(struct sockaddr_in);

    //socket init
    if ((wss->fd = socket(AF_INET, SOCK_STREAM, 0)) <= 0)
    {
        printf("server: create socket failed\r\n");
        return;
    }

    //设置为非阻塞接收
    ret = fcntl(wss->fd, F_GETFL, 0);
    fcntl(wss->fd, F_SETFL, ret | O_NONBLOCK);

    //bind
    count = 0;
    while (bind(wss->fd, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) != 0)
    {
        if (++count > BIND_TIMEOUTMS)
        {
            printf("server: bind timeout %d 服务器端口占用中,请稍候再试\r\n", count);
            goto server_exit;
        }
        ws_delayms(1);
    }

    //listen
    if (listen(wss->fd, 0) != 0)
    {
        printf("server: listen failed\r\n");
        goto server_exit;
    }

    //创建一个epoll控制符
    wss->fd_epoll = epoll_create(CLIENT_MAX);
    //向epoll注册server_sockfd监听事件
    _epoll_ctrl(wss->fd_epoll, wss->fd, EPOLLIN | EPOLLET, EPOLL_CTL_ADD);

    printf("server start \r\n");
    while (!wss->exit)
    {
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时ms
        if ((nfds = epoll_wait(wss->fd_epoll, events, CLIENT_MAX, 500)) < 0)
        {
            printf("server: epoll_wait failed\r\n");
            break;
        }
        for (count = 0; count < nfds; count++)
        {
            //epoll错误
            if ((events[count].events & EPOLLERR) || (events[count].events & EPOLLHUP))
            {
                //从epoll监听列表中移除控制符
                _epoll_ctrl(wss->fd_epoll, events[count].data.fd, 0, EPOLL_CTL_DEL);
                //移除客户端
                client_remove(wss, events[count].data.fd);
            }
            //新通道接入事件
            else if (events[count].data.fd == wss->fd)
            {
                fd_accept = accept(wss->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);
                if (fd_accept >= 0)
                {
                    //创建客户端线程
                    client_create(wss, fd_accept);
                    //添加控制符到epoll监听列表
                    _epoll_ctrl(wss->fd_epoll, fd_accept, 0, EPOLL_CTL_ADD);
                }
            }
            //接收数据事件
            else if (events[count].events & EPOLLIN)
                ;
        }
        //异常客户端检查
        client_check(wss);
    }
    //移除客户端
    client_remove(wss, -1);
    //关闭epoll控制符
    close(wss->fd_epoll);
server_exit:
    //关闭socket
    close(wss->fd);
    wss->exit = true;
    //释放内存
    // free(wss);
}

// ================== 服务器开发需要关心的部分 ===================

/*
 *  接收数据回调
 *  参数:
 *      argv: 客户端信息结构体指针
 *      msg: 接收数据内容
 *      msgLen: >0时为websocket数据包,<0时为非包数据,没有=0的情况
 *      type： websocket包类型
 */
void onMessage(void *argv, char *msg, int msgLen, WsData_Type type)
{
    int ret = 0;
    Ws_Client *wsc = (Ws_Client *)argv;
    //正常 websocket 数据包
    if (msgLen > 0)
    {
        //同时接入客户端数量过大时,不再打印该项,避免影响性能
        if (*wsc->fdTotal < 500)
        {
            printf("onMessage: fd/%03d/%03d recv/%d/%d bytes %s\r\n",
                wsc->fd, *wsc->fdTotal, msgLen, wsc->recvBytes, msgLen < 128 ? msg : " ");
        }
        //在这里根据客户端的请求内容, 提供相应的回复
        if (strstr(msg, "Say hi~") != NULL)
            ;
        //回显,收到什么回复什么
        else
            ret = ws_send(wsc->fd, msg, msgLen, false, type);
        //发送失败,标记异常(后续会被自动回收)
        if (ret < 0)
            wsc->err = true;
    }
    //非 websocket 数据包
    else
    {
        msgLen = -msgLen;
        printf("onMessage: fd/%03d/%03d recv/%d/%d bytes bad pkg %s\r\n",
               wsc->fd, *wsc->fdTotal, msgLen, wsc->recvBytes, msgLen < 128 ? msg : " ");
    }
}

//客户端接入时(已连上),你要做什么?
void onLogin(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    printf("onLogin: fd/%03d/%03d login \r\n", wsc->fd, *wsc->fdTotal);
    //打招呼
    ws_send(wsc->fd, "Say hi~ I am server", 19, false, WDT_TXTDATA);
}

//客户端断开时(已断开),你要做什么?
void onExit(void *argv, int exitType)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    //断开原因
    if (exitType == 0)
        printf("onExit: fd/%03d/%03d disconnect by epoll\r\n", wsc->fd, *wsc->fdTotal);
    else if (exitType == 1)
        printf("onExit: fd/%03d/%03d disconnect by login failed \r\n", wsc->fd, *wsc->fdTotal);
    else if (exitType == 2)
        printf("onExit: fd/%03d/%03d disconnect by pkg WDT_DISCONN \r\n", wsc->fd, *wsc->fdTotal);
    else if (exitType == 3)
        printf("onExit: fd/%03d/%03d disconnect by login timeout \r\n", wsc->fd, *wsc->fdTotal);
}

int main(void)
{
    int i;
    char buff[SEND_PKG_MAX];

    //服务器必须参数
    Ws_Server wss = {
        .port = 9999, //服务器端口
        .path = "/", //服务器路径(这样写表示路径为空)
        .privateData = NULL,//指向自己的数据的指针,回调函数里使用 wsc->privateData 取回
        .onLogin = &onLogin, //客户端接入时(已连上),你要做什么?
        .onExit = &onExit, //客户端断开时(已断开),你要做什么?
        .onMessage = &onMessage, //收到客户端数据时,你要做什么?
    };
    //开辟线程,管理服务器
    new_thread(&wss, &server_thread);

    //服务器启动至少先等3秒(有时会bind超时)
    while (!wss.exit)
    {
        ws_delayms(3000);
        //每3秒推送信息给所有客户端
        for (i = 0; i < CLIENT_MAX; i++)
        {
            if (wss.client[i] && wss.client[i]->login && !wss.client[i]->err)
            {
                snprintf(buff, sizeof(buff), "Tips from server fd/%03d/%03d %s",
                         wss.client[i]->fd, wss.fdTotal, ws_time());

                // if (ws_send(wss.client[i]->fd, buff, sizeof(buff), false, WDT_TXTDATA) < 0) //大数据量压力测试
                if (ws_send(wss.client[i]->fd, buff, strlen(buff), false, WDT_TXTDATA) < 0)
                {
                    //发送失败,标记异常
                    wss.client[i]->err = true;
                }
            }
        }
    }

    wss.exit = true;
    printf("server exit \r\n");
    return 0;
}
