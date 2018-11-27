
#include "websocket_common.h"

#include <stdio.h>
#include <stdlib.h> //exit()
#include <string.h>
#include <errno.h>
#include <unistd.h> //getpid

char ip[24] = {0};//"172.16.23.160";// 本机IP
int port = 9999;

int main(void)
{
    int ret, timeCount = 0;
    int fd;
    char buff[10240];
    int pid;
    //
    pid = getpid();
    printf("\r\n========== client(%d) start ! ==========\r\n\r\n", pid);
    //
    netCheck_getIP("eth0", ip);
    if((fd = webSocket_clientLinkToServer(ip, port, "/null")) <= 0)
    {
        printf("client link to server failed !\r\n");
        return -1;
    }
    webSocket_delayms(100);
    //
    memset(buff, 0, sizeof(buff));
    sprintf(buff, "Say hi~ from client(%d)", pid);
    webSocket_send(fd, buff, strlen(buff), true, WDT_TXTDATA);
    //
    while(1)
    {
        //
        memset(buff, 0, sizeof(buff));
        ret = webSocket_recv(fd, buff, sizeof(buff), NULL);
        if(ret > 0)
        {
            //===== 与服务器的应答 =====
            printf("client(%d) recv : %s\r\n", pid, buff);
            //
            if(strstr(buff, "Hi~") != NULL)
            {
                memset(buff, 0, sizeof(buff));
                sprintf(buff, "I am client(%d)", pid);
                ret = webSocket_send(fd, buff, strlen(buff), true, WDT_TXTDATA);
            }
            else
                ;
            // ......
            // ...
            
            // send返回异常, 连接已断开
            if(ret <= 0)
            {
                close(fd);
                break;
            }
        }
        else    // 检查错误, 是否连接已断开
        {
            if(errno == EAGAIN || errno == EINTR)
                ;
            else
            {
                close(fd);
                break;
            }
        }
        
        //===== 3s客户端心跳 =====
        if(timeCount > 3000)   
        {
            timeCount = 10;
            //
            memset(buff, 0, sizeof(buff));

            // sprintf(buff, "heart from client(%d)", pid);
            // ret = webSocket_send(fd, buff, strlen(buff), true, WDT_TXTDATA);

            strcpy(buff, "123");//即使ping包也要带点数据
            ret = webSocket_send(fd, buff, strlen(buff), true, WDT_PING); //使用ping包代替心跳

            if(ret <= 0)
            {
                close(fd);
                break;
            }
        }
        else
            timeCount += 10;

        //
        webSocket_delayms(10);
    }
    printf("client close !\r\n");
    return 0;
}

