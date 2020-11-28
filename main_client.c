
#include <stdio.h>
#include <stdlib.h> //exit()
#include <string.h>
#include <errno.h>
#include <unistd.h> //getpid

#include "ws_com.h"

//发包数据量 100K
#define SEND_PKG_MAX (10240)

//收包缓冲区大小 100K+
#define RECV_PKG_MAX (SEND_PKG_MAX + 16)

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999

int main(void)
{
    int fd, pid;
    int ret;
    int heart = 0;

    char recv_buff[RECV_PKG_MAX];
    char send_buff[SEND_PKG_MAX];

    //用本进程pid作为唯一标识
    pid = getpid();
    printf("\r\n========== client(%d) start ! ==========\r\n\r\n", pid);

    //2秒超时连接服务器
    if ((fd = ws_connectToServer(SERVER_IP, SERVER_PORT, "/null", 2000)) <= 0)
    {
        printf("client link to server failed ! %d\r\n", fd);
        return -1;
    }

    ws_delayms(100);

    //发出第一条信息
    snprintf(send_buff, sizeof(send_buff), "Say hi~ from client(%d)", pid);
    ws_send(fd, send_buff, strlen(send_buff), true, WDT_TXTDATA);

    //循环接收服务器下发
    while (1)
    {
        ws_delayms(10);

        //接收数据
        ret = ws_recv(fd, recv_buff, sizeof(recv_buff), NULL);
        //正常包
        if (ret > 0)
        {
            printf("client(%d): recv len/%d %s\r\n", pid, ret, recv_buff);

            //根据服务器下发内容做出反应
            if (strstr(recv_buff, "Hi~") != NULL)
            {
                snprintf(send_buff, sizeof(send_buff), "I am client(%d)", pid);
                ret = ws_send(fd, send_buff, strlen(send_buff), true, WDT_TXTDATA);
            }
            
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
        //没有输据接收时,检查错误,是否连接已断开
        else if (errno != EAGAIN && errno != EINTR)
        {
            printf("client(%d): check error %d, disconnect now ...\r\n", pid, errno);
            break;
        }

        //一个合格的客户端,用该定时给服务器发心跳
        heart += 10;
        if (heart > 3000)
        {
            heart = 0;
            //发送心跳
            snprintf(send_buff, sizeof(send_buff), "Heart from client(%d) %s", pid, ws_time());
            // ret = ws_send(fd, send_buff, sizeof(send_buff), true, WDT_TXTDATA); //大数据量压力测试
            ret = ws_send(fd, send_buff, strlen(send_buff), true, WDT_TXTDATA);
            //send返回异常, 连接已断开
            if (ret <= 0)
            {
                printf("client(%d): send failed %d, disconnect now ...\r\n", pid, ret);
                break;
            }
        }
    }

    close(fd);
    printf("client(%d): close\r\n", pid);
    return 0;
}
