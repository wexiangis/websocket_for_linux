
#include "websocket_common.h"

char ip[] = "172.16.23.160";// 本机IP
int port = 9999;

int main(void)
{
    int ret, timeCount = 0;
    int fd;
    char buf[10240];
    //
    fd = webSocket_clientLinkToServer(ip, port, "/null");
    if(fd <= 0)
    {
        printf("client link to server failed !\r\n");
        return -1;
    }
    //
    sleep(1);
    //
    ret = webSocket_send(fd, "Hello !", strlen("Hello !"), true, WDT_TXTDATA);
    //
    printf("\r\n\r\n========== client start ! ==========\r\n\r\n");
    //
    while(1)
    {
        memset(buf, 0, sizeof(buf));
        ret = webSocket_recv(fd, buf, sizeof(buf));
        if(ret > 0)
        {
            printf("client recv : len/%d %s\r\n", ret, buf);
            if(strstr(buf, "Hello") != NULL)
                ret = webSocket_send(fd, "I am Client_Test", strlen("I am Client_Test"), true, WDT_TXTDATA);
            else if(strstr(buf, "Server_Test") != NULL)
                ret = webSocket_send(fd, "I am free now !", strlen("I am free now !"), true, WDT_TXTDATA);
            else
                ;
            //
            if(ret <= 0)    // send返回负数, 连接已断开
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
        //
        webSocket_delayms(10);
        timeCount += 10;
        //
        if(timeCount >= 4000)   /////////////////////////////////////////////////////////////////////////////  每4s 客户端可以在这里定时骚扰一下服务器
        {
            timeCount = 0;
            ret = webSocket_send(fd, "#%^#@@@DTG%^&&+_)+(*^%!HHI", strlen("#%^#@@@DTG%^&&+_)+(*^%!HHI"), true, WDT_TXTDATA);
            if(ret <= 0)
            {
                close(fd);
                break;
            }
         }
    }
    printf("client close !\r\n");
    return 0;
}

