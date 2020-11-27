
#include <stdio.h>
#include <stdlib.h> //exit()
#include <string.h>
#include <errno.h>
#include <unistd.h> //getpid

#include "ws_com.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999

int main(void)
{
    int fd, pid;
    int ret;
    int heartCount = 0;
    char buff[102400];

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
    memset(buff, 0, sizeof(buff));
    sprintf(buff, "Say hi~ from client(%d)", pid);
    ws_send(fd, buff, strlen(buff), true, WDT_TXTDATA);

    //循环接收服务器下发
    while (1)
    {
        ws_delayms(10);

        //接收数据
        memset(buff, 0, sizeof(buff));
        ret = ws_recv(fd, buff, sizeof(buff), NULL);
        if (ret > 0)
        {
            printf("client(%d): recv %s\r\n", pid, buff);

            //根据服务器下发内容做出反应
            if (strstr(buff, "Hi~") != NULL)
            {
                memset(buff, 0, sizeof(buff));
                sprintf(buff, "I am client(%d)", pid);
                ret = ws_send(fd, buff, strlen(buff), true, WDT_TXTDATA);
            }
            else
                ;

            // send返回异常, 连接已断开
            if (ret <= 0)
            {
                printf("client(%d): send failed %d, disconnect now ...\r\n", pid, ret);
                close(fd);
                break;
            }
        }
        // 没有输据接收时,检查错误,是否连接已断开
        else
        {
            if (errno == EAGAIN || errno == EINTR)
                ;
            else
            {
                printf("client(%d): check error %d, disconnect now ...\r\n", pid, errno);
                close(fd);
                break;
            }
        }

        //一个合格的客户端,用该定时给服务器发心跳
        heartCount += 10;
        if (heartCount > 3000)
        {
            heartCount = 0;

            memset(buff, 0, sizeof(buff));
            sprintf(buff, "Heart from client(%d)", pid);
            ret = ws_send(fd, buff, strlen(buff), true, WDT_TXTDATA);

            // strcpy(buff, "123");                                   //即使ping包也要带点数据
            // ret = ws_send(fd, buff, strlen(buff), true, WDT_PING); //使用ping包代替心跳

            // send返回异常, 连接已断开
            if (ret <= 0)
            {
                printf("client(%d): send failed %d, disconnect now ...\r\n", pid, ret);
                close(fd);
                break;
            }
        }
    }

    printf("client(%d): close\r\n", pid);
    return 0;
}
