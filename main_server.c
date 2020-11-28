
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h> //exit()
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h> //非阻塞宏
#include <sys/ioctl.h>
#include <sys/epoll.h> //epoll管理服务器的连接和接收触发
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <pthread.h> //使用多线程

#include "ws_com.h"

//发包数据量 100K
#define SEND_PKG_MAX (10240)

//收包缓冲区大小 100K+
#define RECV_PKG_MAX (SEND_PKG_MAX + 16)

//限制最大可接入客户端数量
#define EPOLL_RESPOND_NUM 1000

//bind超时ms(通常为服务器端口被占用,或者有客户端还连着上次的服务器)
#define SERVER_BIND_TIMEOUTMS 1000

#define SERVER_PORT 9999

typedef struct WsServer
{
    int fd;
    int port;
    bool exit;
    void (*server_callBack)(struct WsServer *wss, int fd, char *buff, int buffLen, WsData_Type type);
    //[x][0]/fd, [x][1]/enable
    int clientArray[EPOLL_RESPOND_NUM][2];
} Ws_Server;

//记录客户端控制符,作为客户端唯一标识
int arrayAdd(int array[][2], int arraySize, int fd)
{
    int i;
    for (i = 0; i < arraySize; i++)
    {
        if (array[i][1] == 0)
        {
            array[i][0] = fd;
            array[i][1] = 1;
            return 0;
        }
    }
    return -1;
}
int arrayRemove(int array[][2], int arraySize, int fd)
{
    int i;
    for (i = 0; i < arraySize; i++)
    {
        if (array[i][0] == fd)
        {
            array[i][0] = 0;
            array[i][1] = 0;
            return 0;
        }
    }
    return -1;
}
int arrayFind(int array[][2], int arraySize, int fd)
{
    int i;
    for (i = 0; i < arraySize; i++)
    {
        if (array[i][0] == fd)
            return i;
    }
    return -1;
}

//抛线程工具
//void throwOut_thread(void *obj, void *callback)
//{
//    pthread_t th;
//    pthread_attr_t attr;
//    //attr init
//    pthread_attr_init(&attr);
//    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED); //禁用线程同步, 线程运行结束后自动释放
//    //抛出线程
//    pthread_create(&th, &attr, callback, (void *)obj);
//    //attr destroy
//    pthread_attr_destroy(&attr);
//}

//接收数据,自动连接客户端,连接异常返回非0
int server_recv(Ws_Server *wss, int fd)
{
    int ret;
    char buff[RECV_PKG_MAX] = {0};
    WsData_Type retPkgType = WDT_NULL;

    ret = ws_recv(fd, buff, sizeof(buff), &retPkgType);

    //这可能是一包客户端请求
    if (ret < 0 &&
        strncmp(buff, "GET", 3) == 0 &&
        strstr(buff, "Sec-WebSocket-Key"))
    {
        //检查该客户端是否未接入过
        if (arrayFind(wss->clientArray, EPOLL_RESPOND_NUM, fd) < 0)
        {
            //构建回复
            ret = ws_responseClient(fd, buff, ret, NULL);
            //连接成功后,再添加fd到数组
            if (ret > 0)
                arrayAdd(wss->clientArray, EPOLL_RESPOND_NUM, fd);
            //发送失败,连接断开
            else
                return -1;
        }
    }
    //无接收数据时,检查连接状态
    else if (ret == 0)
    {
        if (errno > 0 && (errno == EAGAIN || errno == EINTR))
            ;
        else
            return -1;
    }
    //回调
    else if (wss->server_callBack)
        wss->server_callBack(wss, fd, buff, ret, retPkgType);

    return 0;
}

void server_thread(void *argv)
{
    Ws_Server *wss = (Ws_Server *)argv;
    int ret, count;
    int accept_fd;

    socklen_t socAddrLen;
    struct sockaddr_in acceptAddr;
    struct sockaddr_in serverAddr;

    int epoll_fd;
    int nfds;              //epoll监听事件发生的个数
    struct epoll_event ev; //epoll事件结构体
    struct epoll_event events[EPOLL_RESPOND_NUM];

    int bind_timeout = 0;

    memset(&serverAddr, 0, sizeof(serverAddr)); //数据初始化--清零
    serverAddr.sin_family = AF_INET;            //设置为IP通信
    serverAddr.sin_addr.s_addr = INADDR_ANY;    //服务器IP地址
    serverAddr.sin_port = htons(wss->port);     //服务器端口号
    socAddrLen = sizeof(struct sockaddr_in);

    //socket init
    wss->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (wss->fd <= 0)
    {
        printf("server: create socket failed\r\n");
        wss->exit = true;
        return;
    }

    //设置为非阻塞接收
    ret = fcntl(wss->fd, F_GETFL, 0);
    fcntl(wss->fd, F_SETFL, ret | O_NONBLOCK);

    //bind
    while (bind(wss->fd, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) != 0)
    {
        if (++bind_timeout > SERVER_BIND_TIMEOUTMS)
        {
            printf("server bind timeout %d 服务器端口占用中,请稍候再试\r\n", bind_timeout);
            wss->exit = true;
            return;
        }
        ws_delayms(1);
    }

    //listen
    if (listen(wss->fd, 0) != 0)
    {
        printf("server listen failed\r\n");
        close(wss->fd);
        wss->exit = true;
        return;
    }

    //创建一个epoll控制符
    epoll_fd = epoll_create(EPOLL_RESPOND_NUM);
    if (epoll_fd < 0)
    {
        printf("server epoll_create failed\r\n");
        close(wss->fd);
        wss->exit = true;
        return;
    }

    //向epoll注册server_sockfd监听事件
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = wss->fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wss->fd, &ev) < 0)
    {
        printf("server epll_ctl : wss->fd register failed\r\n");
        close(epoll_fd);
        wss->exit = true;
        return;
    }

    printf("\r\n\r\n========== server start ! ==========\r\n\r\n");
    while (!wss->exit)
    {
        //等待事件发生, -1表示阻塞、其它数值为超时
        nfds = epoll_wait(epoll_fd, events, EPOLL_RESPOND_NUM, -1);
        if (nfds < 0)
        {
            printf("server start epoll_wait failed\r\n");
            close(epoll_fd);
            return;
        }
        for (count = 0; count < nfds; count++)
        {
            //epoll错误
            if ((events[count].events & EPOLLERR) || (events[count].events & EPOLLHUP))
            {
                printf("server: accept close fd/%03d\r\n", events[count].data.fd);
                ev.data.fd = events[count].data.fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[count].data.fd, &ev) < 0)
                {
                    printf("server: EPOLL_CTL_DEL failed !\r\n");
                    // close(epoll_fd);
                    // return;
                }
                //移除客户端
                arrayRemove(wss->clientArray, EPOLL_RESPOND_NUM, events[count].data.fd);
                close(events[count].data.fd);
            }

            //新通道接入事件
            else if (events[count].data.fd == wss->fd)
            {
                accept_fd = accept(wss->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);
                if (accept_fd >= 0)
                {
                    ev.data.fd = accept_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accept_fd, &ev) < 0)
                    {
                        printf("server epoll_ctl : EPOLL_CTL_ADD failed\r\n");
                        close(epoll_fd);
                        // return;
                    }
                    printf("server: accept fd/%03d\r\n", accept_fd);
                }
            }

            //接收数据事件
            else if (events[count].events & EPOLLIN)
            {
                if (server_recv(wss, events[count].data.fd) != 0)
                {
                    printf("server: check error fd/%03d %d\r\n", events[count].data.fd, errno);
                    ev.data.fd = events[count].data.fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[count].data.fd, &ev) < 0)
                    {
                        printf("server: EPOLL_CTL_DEL failed\r\n");
                        // close(epoll_fd);
                        // return;
                    }
                    arrayRemove(wss->clientArray, EPOLL_RESPOND_NUM, events[count].data.fd); //从数组剔除fd
                    close(events[count].data.fd);
                }
            }

            //发送数据事件
            else if (events[count].events & EPOLLOUT)
                ;
        }
    }
    //关闭epoll控制符
    close(epoll_fd);
    //关闭socket
    close(wss->fd);
}

//客户端数据接收回调
void server_callBack(Ws_Server *wss, int fd, char *buff, int buffLen, WsData_Type type)
{
    int ret = 0;
    //正常 websocket 数据包
    if (buffLen > 0)
    {
        printf("server: recv fd/%03d len/%d %s\r\n", fd, buffLen, buff);

        //在这里根据客户端的请求内容, 提供相应的回复
        if (strstr(buff, "hi~") != NULL)
            ret = ws_send(fd, "Hi~ I am server", 15, false, WDT_TXTDATA);
        //回显
        else
            ret = ws_send(fd, buff, strlen(buff), false, WDT_TXTDATA);

        //发送失败,连接断开
        if (ret < 0)
            ;
    }
    //非 websocket 数据包
    else
    {
        printf("server: recv fd/%03d len/%d bad pkg %s\r\n", fd, buffLen, buff);
    }
}

int main(void)
{
    int i;
    int client_fd;
    char buff[SEND_PKG_MAX];

    pthread_t sever_thread_id;
    Ws_Server wss;

    //初始化服务器参数
    memset(&wss, 0, sizeof(wss));
    wss.port = SERVER_PORT;
    //收到客户端数据时,需要干嘛?
    wss.server_callBack = &server_callBack;

    //开辟线程,管理服务器
    if (pthread_create(&sever_thread_id, NULL, (void *)&server_thread, (void *)(&wss)) != 0)
    {
        printf("create server false !\r\n");
        wss.exit = true;
    }

    while (!wss.exit)
    {
        ws_delayms(3000);

        //每3秒推送信息给所有客户端
        for (i = 0; i < EPOLL_RESPOND_NUM; i++)
        {
            if (wss.clientArray[i][1] != 0 && wss.clientArray[i][0] > 0)
            {
                client_fd = wss.clientArray[i][0];
                snprintf(buff, sizeof(buff), "Tips from server fd/%03d %s", client_fd, ws_time());
                // if (ws_send(client_fd, buff, sizeof(buff), false, WDT_TXTDATA) < 0) //大数据量压力测试
                if (ws_send(client_fd, buff, strlen(buff), false, WDT_TXTDATA) < 0)
                {
                    printf("server: send fd/%03d error\r\n", client_fd);
                    //标记损坏
                    wss.clientArray[i][1] = 0;
                }
            }
        }
    }

    wss.exit = true;
    pthread_cancel(sever_thread_id); //等待线程关闭
    printf("server close !\r\n");
    return 0;
}
