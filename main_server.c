
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

/*
 *  websocket服务器通常有ip、port和路径3个参数
 *  写作"ws://ip:port/aaa/bb/cc",其中"/aaa/bb/cc"即为路径
 *  这里未作检查
 */
#define SERVER_PATH "/" //"/aaa/bb/cc"

#define SERVER_PORT 9999

//发包数据量 10K
#define SEND_PKG_MAX (10240)

//收包缓冲区大小 10K+
#define RECV_PKG_MAX (SEND_PKG_MAX + 16)

//限制最大可接入客户端数量
#define CLIENT_MAX 1000

//bind超时ms(通常为服务器端口被占用,或者有客户端还连着上次的服务器)
#define BIND_TIMEOUTMS 1000

//客户端数据接收回调函数原型
typedef void (*RecvCallback)(void *obj, char *buff, int len, WsData_Type type);

//客户端线程使用的参数结构体
typedef struct WsClient
{
    int fd; //accept之后得到的客户端连接控制符
    int *fdTotal; //当前接入客户端总数
    bool login; //是否完成websocket登录验证
    bool exit; //线程结束标志
    bool err; //异常标志
    unsigned int recvBytes; //总接收字节计数
    RecvCallback recvCallback;
} Ws_Client;

//服务器线程使用的参数结构体
typedef struct WsServer
{
    int fd; //服务器控制符
    int fdTotal; //当前接入客户端总数
    int port;
    bool exit; //线程结束标志
    Ws_Client *client[CLIENT_MAX]; //当前接入客户端列表
    RecvCallback recvCallback;
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

/*
 *  接收数据,自动连接客户端
 *  返回: 
 *      >0 有数据
 *      =0 无数据
 *      <0 异常
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
        ret = ws_responseClient(wsc->fd, buff, ret, SERVER_PATH);
        //连接成功,标记登录
        if (ret > 0)
        {
            printf("server: fd/%03d/%03d login \r\n", wsc->fd, *wsc->fdTotal);
            wsc->login = true;
            return 0;
        }
        //发送失败,连接断开
        else
            return -1;
    }
    //接收数据量统计
    if (wsc->login && ret != 0)
        wsc->recvBytes += ret > 0 ? ret : (-ret);

    //收到特殊包
    if (retPkgType == WDT_DISCONN)
    {
        printf("server: fd/%03d/%03d recv WDT_DISCONN \r\n", wsc->fd, *wsc->fdTotal);
        return -1;
    }
    else if (retPkgType == WDT_PING)
        printf("server: fd/%03d/%03d recv WDT_PING \r\n", wsc->fd, *wsc->fdTotal);
    else if (retPkgType == WDT_PONG)
        printf("server: fd/%03d/%03d recv WDT_PONG \r\n", wsc->fd, *wsc->fdTotal);
    //回调
    else if (ret != 0 && wsc->recvCallback)
        wsc->recvCallback(wsc, buff, ret, retPkgType);
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
    char exitType = 0;             //客户端断开原因

    printf("server: fd/%03d/%03d start \r\n", wsc->fd, *wsc->fdTotal);

    while (!wsc->exit)
    {
        //周期接收,每次收完为止
        do
        {
            ret = client_recv(wsc);
        } while (ret > 0);
        //收包异常
        if (ret < 0)
        {
            exitType = 1;
            break;
        }
        //连接后半天不登录,踢出客户端
        if (!wsc->login)
        {
            loginTimeout += intervalMs;
            if (loginTimeout > 5000)
            {
                exitType = 2;
                break;
            }
        }
        ws_delayms(intervalMs);
    }
    //断开原因
    if (exitType == 0)
        printf("server: fd/%03d/%03d disconnect \r\n", wsc->fd, *wsc->fdTotal);
    else if (exitType == 1)
        printf("server: fd/%03d/%03d disconnect by recv error \r\n", wsc->fd, *wsc->fdTotal);
    else if (exitType == 2)
        printf("server: fd/%03d/%03d disconnect by login timeout \r\n", wsc->fd, *wsc->fdTotal);
    //关闭控制符,释放内存
    close(wsc->fd);
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
            wss->client[i]->recvCallback = wss->recvCallback;
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

//服务器主线程,负责检测 新客户端接入 及 客户端断开
void server_thread(void *argv)
{
    Ws_Server *wss = (Ws_Server *)argv;
    int ret, count;
    int accept_fd;

    socklen_t socAddrLen;
    struct sockaddr_in acceptAddr;
    struct sockaddr_in serverAddr = {0};

    int epoll_fd;
    int nfds;
    struct epoll_event ev;
    struct epoll_event events[CLIENT_MAX];

    serverAddr.sin_family = AF_INET;         //设置为IP通信
    serverAddr.sin_addr.s_addr = INADDR_ANY; //服务器IP地址
    serverAddr.sin_port = htons(wss->port);  //服务器端口号
    socAddrLen = sizeof(struct sockaddr_in);

    //socket init
    wss->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (wss->fd <= 0)
    {
        printf("server: create socket failed\r\n");
        goto server_exit0;
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
            goto server_exit0;
        }
        ws_delayms(1);
    }

    //listen
    if (listen(wss->fd, 0) != 0)
    {
        printf("server: listen failed\r\n");
        goto server_exit1;
    }

    //创建一个epoll控制符
    if ((epoll_fd = epoll_create(CLIENT_MAX)) < 0)
    {
        printf("server: epoll_create failed\r\n");
        goto server_exit1;
    }

    //向epoll注册server_sockfd监听事件
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = wss->fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wss->fd, &ev) < 0)
    {
        printf("server: epll_ctl register failed\r\n");
        goto server_exit2;
    }

    printf("server start \r\n");
    while (!wss->exit)
    {
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时
        if ((nfds = epoll_wait(epoll_fd, events, CLIENT_MAX, -1)) < 0)
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
                ev.data.fd = events[count].data.fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[count].data.fd, &ev);
                //移除客户端
                client_remove(wss, events[count].data.fd);
            }

            //新通道接入事件
            else if (events[count].data.fd == wss->fd)
            {
                accept_fd = accept(wss->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);
                if (accept_fd >= 0)
                {
                    //创建客户端线程
                    client_create(wss, accept_fd);
                    //添加控制符到epoll监听列表
                    ev.data.fd = accept_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accept_fd, &ev);
                }
            }
            //接收数据事件
            else if (events[count].events & EPOLLIN)
                ;
        }
    }

    //移除客户端
    client_remove(wss, -1);
server_exit2:
    //关闭epoll控制符
    close(epoll_fd);
server_exit1:
    //关闭socket
    close(wss->fd);
server_exit0:
    wss->exit = true;
    //释放内存
    // free(wss);
}

/*
 *  客户端数据接收回调
 *  参数:
 *      wsc: 
 *      buff: 解包后的最终数据
 *      buffLen: 数据量, 为负值时表示非标准包的数据量
 *      type: 包类型
 */
void recvCallback(void *argv, char *buff, int len, WsData_Type type)
{
    int ret = 0;
    Ws_Client *wsc = (Ws_Client *)argv;
    //正常 websocket 数据包
    if (len > 0)
    {
        printf("server: fd/%03d/%03d recv/%d/%d bytes %s\r\n",
               wsc->fd, *wsc->fdTotal, len, wsc->recvBytes, len < 128 ? buff : " ");

        //在这里根据客户端的请求内容, 提供相应的回复
        if (strstr(buff, "hi~") != NULL)
            ret = ws_send(wsc->fd, "hi~ I am server", 15, false, WDT_TXTDATA);
        //回显,收到什么回复什么
        else
            ret = ws_send(wsc->fd, buff, len, false, type);

        //发送失败,标记异常
        if (ret < 0)
            wsc->err = true;
    }
    //非 websocket 数据包
    else
        printf("server: fd/%03d/%03d recv/%d/%d bytes bad pkg %s\r\n",
               wsc->fd, *wsc->fdTotal, len, wsc->recvBytes, len < 128 ? buff : " ");
}

int main(void)
{
    int i;
    char buff[SEND_PKG_MAX];
    Ws_Server wss = {0};

    //初始化服务器参数
    wss.port = SERVER_PORT;
    //收到客户端数据时,需要干嘛?
    wss.recvCallback = &recvCallback;
    //开辟线程,管理服务器
    new_thread(&wss, &server_thread);

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
                    //发送失败,标记损坏
                    wss.client[i]->err = true;
                }
            }
        }
    }

    wss.exit = true;
    printf("server close !\r\n");
    return 0;
}
