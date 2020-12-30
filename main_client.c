
#include <stdio.h>
#include <stdlib.h> //exit()
#include <string.h>
#include <errno.h>
#include <unistd.h> //getpid

#include "ws_com.h"

//发包数据量 10K
#define SEND_PKG_MAX (10240)

//收包缓冲区大小 10K+
#define RECV_PKG_MAX (SEND_PKG_MAX + 16)

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999
#define SERVER_PATH "/"

#if 1 // 发收包测试

int main(void)
{
    int fd, pid;
    int ret;
    int heart = 0;
    WsData_Type retPkgType;

    char recv_buff[RECV_PKG_MAX];
    char send_buff[SEND_PKG_MAX];

    //用本进程pid作为唯一标识
    pid = getpid();
    printf("client start pid/%d \r\n", pid);

    //3秒超时连接服务器
    //同时大量接入时,服务器不能及时响应,可以加大超时时间
    if ((fd = ws_connectToServer(SERVER_IP, SERVER_PORT, "/null", 3000)) <= 0)
    {
        printf("client link to server failed ! %d\r\n", fd);
        return -1;
    }

    //循环接收服务器下发
    while (1)
    {
        //接收数据
        ret = ws_recv(fd, recv_buff, sizeof(recv_buff), &retPkgType);
        //正常包
        if (ret > 0)
        {
            printf("client(%d): recv len/%d %s\r\n", pid, ret, recv_buff);

            //根据服务器下发内容做出反应
            if (strstr(recv_buff, "Say hi~") != NULL)
            {
                snprintf(send_buff, sizeof(send_buff), "Say hi~ I am client(%d)", pid);
                ret = ws_send(fd, send_buff, strlen(send_buff), true, WDT_TXTDATA);
            }
            else if (strstr(recv_buff, "I am ") != NULL)
                ;

            //send返回异常, 连接已断开
            if (ret <= 0)
            {
                printf("client(%d): send failed %d, disconnect now ...\r\n", pid, ret);
                break;
            }
        }
        //非包数据
        else if (ret < 0)
            printf("client(%d): recv len/%d bad pkg %s\r\n", pid, -ret, recv_buff);
        else
        {
            //收到特殊包
            if (retPkgType == WDT_DISCONN)
            {
                printf("client(%d): recv WDT_DISCONN \r\n", pid);
                break;
            }
            else if (retPkgType == WDT_PING)
                printf("client(%d): recv WDT_PING \r\n", pid);
            else if (retPkgType == WDT_PONG)
                printf("client(%d): recv WDT_PONG \r\n", pid);
        
            //没有输据接收时,检查错误,是否连接已断开
            else if (errno != EAGAIN && errno != EINTR)
            {
                printf("client(%d): check error %d, disconnect now ...\r\n", pid, errno);
                break;
            }
        }

        //一个合格的客户端,应该定时给服务器发心跳
        heart += 10;
        if (heart > 3000)
        {
            heart = 0;
#if 1
            //发送心跳
            snprintf(send_buff, sizeof(send_buff), "Heart from client(%d) %s", pid, ws_time());
            // ret = ws_send(fd, send_buff, sizeof(send_buff), true, WDT_TXTDATA); //大数据量压力测试
            ret = ws_send(fd, send_buff, strlen(send_buff), true, WDT_TXTDATA);
#else
            //用ping包代替心跳
            ret =  ws_send(fd, NULL, 0, true, WDT_PING);
#endif
            //send返回异常, 连接已断开
            if (ret <= 0)
            {
                printf("client(%d): send failed %d, disconnect now ...\r\n", pid, ret);
                break;
            }
        }
        
        ws_delayms(10);
    }

    close(fd);
    printf("client(%d): close\r\n", pid);
    return 0;
}

#else // 利用服务器回显,发收文件测试

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

//指定要读取的文件
#define FILE_R "./in.bin"
//指定要写入的文件
#define FILE_W "./out.bin"

int main(void)
{
    int ret, recvTotal = 0, sendTotal = 0, timeout = 0;
    WsData_Type type;
    char recv_buff[RECV_PKG_MAX];
    char send_buff[SEND_PKG_MAX];
    int fd, fr, fw;
    char frOver = 0, fwOver = 0;

    fd = ws_connectToServer(SERVER_IP, SERVER_PORT, "/null", 2000);
    if (fd < 1)
    {
        printf("connect failed \r\n");
        return 1;
    }

    fr = open(FILE_R, O_RDONLY);
    if (fr < 1)
    {
        printf("open FILE_R %s failed \r\n", FILE_R);
        goto exit_ws;
    }

    fw = open(FILE_W, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fw < 1)
    {
        printf("open FILE_W %s failed \r\n", FILE_W);
        goto exit_fr;
    }

    while (!frOver || !fwOver)
    {
        //读文件,发数据
        if (!frOver)
        {
            ret = read(fr, send_buff, sizeof(send_buff));
            if (ret > 0)
            {
                timeout = 0;
                sendTotal += ret;
                ret = ws_send(fd, send_buff, ret, true, WDT_BINDATA);
            }
            else
                frOver = 1;
        }
        //收数据
        do
        {
            ret = ws_recv(fd, recv_buff, sizeof(recv_buff), &type);
            // type == WDT_BINDATA 暂且区分服务器服务器下发的其它推送内容,避免和文件数据混淆
            if (ret > 0 && type == WDT_BINDATA)
            {
                timeout = 0;
                recvTotal += ret;
                printf("recv/%d total/%d send/%d bytes\r\n", ret, recvTotal, sendTotal);
                write(fw, recv_buff, ret);
            }
            else if (ret < 0)
            {
                timeout = 0;
                recvTotal += -ret;
                printf("recv/%d total/%d send/%d bytes bad pkg\r\n", ret, recvTotal, sendTotal);
            }
            else if (frOver)
            {
                timeout += 100;
                //200ms超时
                if (timeout > 200000 || recvTotal >= sendTotal)
                {
                    fwOver = 1;
                    //主动断连
                    ws_send(fd, NULL, 0, false, WDT_DISCONN);
                }
                break;
            }
            else
                timeout = 0;
        } while (ret != 0);

        ws_delayus(100);
    }

    close(fw);
exit_fr:
    close(fr);
exit_ws:
    close(fd);

    return 0;
}

#endif