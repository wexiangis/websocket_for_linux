
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

//最大副线程数量(不包括负责accept的主线程)
//计算机线程数量有限,建议限制在300以内(或上网搜索"linux最大线程数量"进一步了解)
#define THREAD_MAX 10

//每个副线程维护客户端最大数量
#define CLIENT_OF_THREAD 500

/*
 *  接入客户端最大数量
 * 
 *  限制接入量的"性能因素":
 *      1.普通计算机接入破千之后CPU会逐渐拉满,很难再接入/发起更多客户端
 *      2.服务端表现为接入量上涨变缓
 *      3.客户端表现为connect阶段超时、登录阶段超时等
 *
 *  限制接入量的"参数因素":
 *      1.线程数量上限,上面宏定义 THREAD_MAX 中有提到
 *      2.文件描述符上限,使用指令“ulimit -a”在"open files"项可见,一般为4096,可尝试用指令"ulimit -n 8192"提高
 */
#define CLIENT_MAX (THREAD_MAX * CLIENT_OF_THREAD) // 10*500=5000

//bind超时,通常为服务器端口被占用,或者有客户端还连着上次的服务器
#define BIND_TIMEOUT_MS 1000

//连接后又不进行websocket握手,5秒超时踢出
#define LOGIN_TIMEOUT_MS 5000

//接入客户端数量超出这个数时不在 onMessage 打印,避免卡顿
#define CLIENT_OF_PRINTF 500

//断连原因
typedef enum
{
    WET_NONE = 0,
    WET_EPOLL, //epoll检测
    WET_SEND, //发送失败
    WET_LOGIN, //websocket握手检查失败(http请求格式错误或者path值不一致)
    WET_LOGIN_TIMEOUT, //连接后迟迟不websocket握手
    WET_PKG_DIS, //收到断开协议包
} Ws_ExitType;

//客户端事件回调函数原型
typedef void (*OnLogin)(void *obj);
typedef void (*OnMessage)(void *obj, char *msg, int msgLen, WsData_Type type);
typedef void (*OnExit)(void *obj, Ws_ExitType exitType);

typedef struct WsClient Ws_Client;
typedef struct WsThread Ws_Thread;
typedef struct WsServer Ws_Server;

//服务器副线程,负责检测 数据接收 和 客户端断开
void server_thread2(void *argv);
//服务器主线程,负责检测 新客户端接入
void server_thread(void *argv);

//客户端使用的参数结构体
struct WsClient
{
    int fd; //accept之后得到的客户端连接描述符
    Ws_ExitType exitType; //断连标志
    bool isLogin; //是否完成websocket握手验证
    bool isExiting; //正在退出(防止反复del)
    unsigned int recvBytes; //总接收字节计数
    unsigned int order; //接入客户端的历史序号(从1数起)
    unsigned int loginTimeout; //等待websocket握手超时计数
    Ws_Server *wss; //所在服务器指针
    Ws_Thread *wst; //所在副线程指针
};

//副线程结构体(只要还有一个客户端在维护就不会退出线程)
struct WsThread
{
    int fd_epoll; //epoll描述符
    int clientCount; //该线程正在维护的客户端数量
    bool isRun; //线程运行状况
    Ws_Server *wss;
};

//服务器主线程使用的参数结构体
struct WsServer
{
    int fd; //服务器描述符
    int fd_epoll; //epoll描述符
    int port; //服务器端口
    char path[128]; //服务器路径
    void *privateData; //用指针传递自己的数据到回调函数里使用,而不是全局变量
    int clientCount; //当前接入客户端总数
    bool isExit; //线程结束标志
    OnLogin onLogin;
    OnMessage onMessage;
    OnExit onExit;
    Ws_Thread thread[THREAD_MAX]; //副线程数组
    Ws_Client client[CLIENT_MAX]; //全体客户端列表
};

//抛线程工具
void new_thread(void *obj, void *callback)
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
        printf("pthread_create failed !! %s\r\n", strerror(ret));
    //attr destroy
    pthread_attr_destroy(&attr);
}

//注意在"epoll_wait时"或"目标fd已close()"的情况下会ctrl会失败
void _epoll_ctrl(int fd_epoll, int fd, uint32_t event, int ctrl, void *ptr)
{
    struct epoll_event ev;
    ev.events = event;
    if (ptr)
        ev.data.ptr = ptr;
    else
        ev.data.fd = fd;
    if (epoll_ctl(fd_epoll, ctrl, fd, &ev) != 0)
        printf("epoll ctrl %d error !!\r\n", ctrl);
}

/*
 *  接收数据,自动连接客户端
 *  返回: 
 *      >0 有数据
 *      =0 无数据
 *      -1 连接异常
 */
int client_recv(Ws_Client *wsc)
{
    int ret;
    char buff[RECV_PKG_MAX] = {0};
    WsData_Type retPkgType = WDT_NULL;

    ret = ws_recv(wsc->fd, buff, sizeof(buff), &retPkgType);

    //这可能是一包客户端请求
    if (!wsc->isLogin && ret < 0 &&
        strncmp(buff, "GET", 3) == 0 &&
        strstr(buff, "Sec-WebSocket-Key"))
    {
        //构建回复
        if (ws_responseClient(wsc->fd, buff, ret, wsc->wss->path) > 0)
        {
            //这个延时很有必要,否则下面onLogin里面发东西客户端可能收不到
            ws_delayms(5);
            wsc->isLogin = true;
            //回调
            if (wsc->wss->onLogin)
                wsc->wss->onLogin(wsc);
            return 0;
        }
        //登录失败失败,连接断开
        else
        {
            wsc->exitType = WET_LOGIN;
            return -1;
        }
    }
    //接收数据量统计
    if (wsc->isLogin && ret != 0)
        wsc->recvBytes += ret > 0 ? ret : (-ret);
    //收到特殊包(由于这些比较底层,所以没有放到onMessage事件)
    if (retPkgType == WDT_DISCONN)
    {
        wsc->exitType = WET_PKG_DIS;
        printf("specialPkg: fd/%03d order/%03d total/%03d recv WDT_DISCONN \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
        return -1;
    }
    else if (retPkgType == WDT_PING)
        printf("specialPkg: fd/%03d order/%03d total/%03d recv WDT_PING \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    else if (retPkgType == WDT_PONG)
        printf("specialPkg: fd/%03d order/%03d total/%03d recv WDT_PONG \r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    //回调
    else if (ret != 0 && wsc->wss->onMessage)
        wsc->wss->onMessage(wsc, buff, ret, retPkgType);
    //正常返回
    return ret == 0 ? 0 : 1;
}

//onMessage异步回调
void client_onMessage(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    int ret = 1;
    //收完为止
    while (ret > 0)
        ret = client_recv(wsc);
}

//onExit异步回调
void client_onExit(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    if (wsc->wss->onExit)
        wsc->wss->onExit(wsc, wsc->exitType);
    //重置结构体,给下次使用
    memset(wsc, 0, sizeof(Ws_Client));
}

//取得空闲的坑
Ws_Client *client_get(Ws_Server *wss, int fd)
{
    int i;
    for (i = 0; i < CLIENT_MAX; i++)
    {
        if (!wss->client[i].fd &&
            !wss->client[i].isExiting &&
            !wss->client[i].wst)
        {
            memset(&wss->client[i], 0, sizeof(Ws_Client));
            wss->client[i].fd = fd; //占用
            wss->client[i].wss = wss;
            return &wss->client[i];
        }
    }
    printf("client_get failed !!\r\n"); //满员
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
void client_add(Ws_Server *wss, int fd)
{
    int i;
    Ws_Thread *wst = wss->thread;
    //取得空闲客户端指针
    Ws_Client *wsc = client_get(wss, fd);
    if (!wsc)
        return;
    //遍历线程,谁有空谁托管
    for (i = 0; i < THREAD_MAX; i++)
    {
        if (wst[i].clientCount < CLIENT_OF_THREAD && //线程未满员
            wst[i].isRun && //线程在运行
            wst[i].fd_epoll) //线程epoll正常
        {
            //共用代码块
            COMMON_CODE();
            return;
        }
    }
    //开启新线程
    for (i = 0; i < THREAD_MAX; i++)
    {
        if (!wst[i].isRun &&
            wst[i].fd_epoll == 0 &&
            wst[i].clientCount == 0)
        {
            //参数初始化
            wst[i].wss = wss;
            wst[i].fd_epoll = epoll_create(CLIENT_OF_THREAD);
            //开线程
            new_thread(&wst[i], &server_thread2);
            //共用代码块
            COMMON_CODE();
            return;
        }
    }
    printf("client_add failed !!\r\n"); //线程负荷已满
    memset(wsc, 0, sizeof(Ws_Client)); //释放占用的坑
}

//移除特定客户端
void client_del(Ws_Thread *wst, Ws_Client *wsc)
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
void client_detect(Ws_Thread *wst, bool delAll)
{
    Ws_Client *client = wst->wss->client;
    int i;
    for (i = 0; i < CLIENT_MAX; i++)
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
                if (client[i].loginTimeout > LOGIN_TIMEOUT_MS)
                {
                    client[i].exitType = WET_LOGIN_TIMEOUT;
                    client_del(wst, &client[i]);
                }
            }
        }
    }
}

//服务器副线程,负责检测 数据接收 和 客户端断开
void server_thread2(void *argv)
{
    Ws_Thread *wst = (Ws_Thread *)argv;
    int nfds, count;
    struct epoll_event events[CLIENT_OF_THREAD];

    while (!wst->wss->isExit && wst->clientCount > 0)
    {
        wst->isRun = true;
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时ms
        if ((nfds = epoll_wait(wst->fd_epoll, events, CLIENT_OF_THREAD, 500)) < 0)
        {
            printf("server_thread2: epoll_wait failed\r\n");
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

    serverAddr.sin_family = AF_INET; //设置为IP通信
    serverAddr.sin_addr.s_addr = INADDR_ANY; //服务器IP地址
    serverAddr.sin_port = htons(wss->port); //服务器端口号
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
        if (++count > BIND_TIMEOUT_MS)
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

    //创建一个epoll描述符
    wss->fd_epoll = epoll_create(CLIENT_MAX);

    //向epoll注册server_sockfd监听事件
    _epoll_ctrl(wss->fd_epoll, wss->fd, EPOLLIN | EPOLLET, EPOLL_CTL_ADD, NULL);

    printf("server start \r\n");
    while (!wss->isExit)
    {
        //等待事件发生,-1阻塞,0/非阻塞,其它数值为超时ms
        if ((nfds = epoll_wait(wss->fd_epoll, events, CLIENT_MAX, 500)) < 0)
        {
            printf("server_thread: epoll_wait failed\r\n");
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
server_exit:
    //关闭socket
    close(wss->fd);
    wss->isExit = true;
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
        //客户端过多时不再打印,避免卡顿
        if (wsc->wss->clientCount <= CLIENT_OF_PRINTF)
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
    else
    {
        msgLen = -msgLen;
        printf("onMessage: fd/%03d order/%03d total/%03d %d/%dbytes bad pkg %s\r\n",
               wsc->fd, wsc->order, wsc->wss->clientCount, msgLen, wsc->recvBytes, msgLen < 128 ? msg : " ");
    }
}

//客户端接入时(已连上),你要做什么?
void onLogin(void *argv)
{
    Ws_Client *wsc = (Ws_Client *)argv;
    printf("onLogin: fd/%03d order/%03d total/%03d\r\n", wsc->fd, wsc->order, wsc->wss->clientCount);
    //打招呼
    ws_send(wsc->fd, "Say hi~ I am server", 19, false, WDT_TXTDATA);
}

//客户端断开时(已断开),你要做什么?
void onExit(void *argv, Ws_ExitType exitType)
{
    Ws_Client *wsc = (Ws_Client *)argv;
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
    char buff[SEND_PKG_MAX];

    //服务器必须参数
    Ws_Server wss = {
        .port = 9999, //服务器端口
        .path = "/", //服务器路径(这样写表示路径为空)
        .privateData = NULL, //指向自己的数据的指针,回调函数里使用 wsc->privateData 取回
        .onLogin = &onLogin, //客户端接入时(已连上),你要做什么?
        .onMessage = &onMessage, //收到客户端数据时,你要做什么?
        .onExit = &onExit, //客户端断开时(已断开),你要做什么?
    };

    //传参接收
    if (argc > 1) {
        sscanf(argv[1], "%d", &wss.port);
    }
    if (argc > 2) {
        memset(wss.path, 0, sizeof(wss.path));
        strcpy(wss.path, argv[2]);
    }

    //开辟线程,管理服务器
    new_thread(&wss, &server_thread);

    //服务器启动至少先等3秒(有时会bind超时)
    while (!wss.isExit)
    {
        ws_delayms(3000);
        //每3秒推送信息给所有客户端
        for (i = 0; i < CLIENT_MAX; i++)
        {
            if (wss.client[i].fd && wss.client[i].isLogin && !wss.client[i].exitType)
            {
                snprintf(buff, sizeof(buff), "Tips from server fd/%03d order/%03d total/%03d %s",
                         wss.client[i].fd, wss.client[i].order, wss.clientCount, ws_time());

                // if (ws_send(wss.client[i].fd, buff, sizeof(buff), false, WDT_TXTDATA) < 0) //大数据量压力测试
                if (ws_send(wss.client[i].fd, buff, strlen(buff), false, WDT_TXTDATA) < 0)
                {
                    //发送失败,标记异常
                    wss.client[i].exitType = WET_SEND;
                }
            }
        }
    }

    wss.isExit = true;
    printf("server exit \r\n");
    return 0;
}
