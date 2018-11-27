
#include "websocket_common.h"

#include <stdio.h>
#include <stdlib.h>     //exit()
#include <string.h>
#include <errno.h>
#include <fcntl.h>      // 非阻塞宏
#include <sys/ioctl.h>
#include <sys/epoll.h>  // epoll管理服务器的连接和接收触发
#include <sys/socket.h>
#include <netinet/ip.h>
#include <pthread.h>    // 使用多线程

#define		EPOLL_RESPOND_NUM		10000	// epoll最大同时管理句柄数量

typedef int (*CallBackFun)(int fd, char *buf, unsigned int bufLen);

typedef struct{
    int fd;
    int client_fd_array[EPOLL_RESPOND_NUM][2];
    char ip[24];
    int port;
    char buf[10240];
    CallBackFun callBack;
}WebSocket_Server;

// Tool Function
int arrayAddItem(int array[][2], int arraySize, int value)
{
    int i;
    for(i = 0; i < arraySize; i++)
    {
        if(array[i][1] == 0)
        {
            array[i][0] = value;
            array[i][1] = 1;
            return 0;
        }
    }
    return -1;
}

int arrayRemoveItem(int array[][2], int arraySize, int value)
{
    int i;
    for(i = 0; i < arraySize; i++)
    {
        if(array[i][0] == value)
        {
            array[i][0] = 0;
            array[i][1] = 0;
            return 0;
        }
    }
    return -1;
}

// Server Function
void server_thread_fun(void *arge)
{
	int ret , i , j;
	int accept_fd;
	int socAddrLen;
	struct sockaddr_in acceptAddr;
	struct sockaddr_in serverAddr;
	//
	WebSocket_Server *wss = (WebSocket_Server *)arge;
	//
	memset(&serverAddr , 0 , sizeof(serverAddr)); 	// 数据初始化--清零     
    serverAddr.sin_family = AF_INET; 				// 设置为IP通信     
    //serverAddr.sin_addr.s_addr = inet_addr(wss->ip);// 服务器IP地址
    serverAddr.sin_addr.s_addr = INADDR_ANY;		// 服务器IP地址
    serverAddr.sin_port = htons(wss->port); 		// 服务器端口号    
	//
	socAddrLen = sizeof(struct sockaddr_in);
	
	//------------------------------------------------------------------------------ socket init
	//socket init
	wss->fd = socket(AF_INET, SOCK_STREAM,0);  
    if(wss->fd <= 0)  
    {  
        printf("server cannot create socket !\r\n"); 
        exit(1);
    }    
    
    //设置为非阻塞接收
    ret = fcntl(wss->fd , F_GETFL , 0);
    fcntl(wss->fd , F_SETFL , ret | O_NONBLOCK);
    
    //bind sockfd & addr  
    while(bind(wss->fd, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) < 0 )
        webSocket_delayms(1);
    
    //listen sockfd   
    ret = listen(wss->fd, 0);  
    if(ret < 0) 
    {  
        printf("server cannot listen request\r\n"); 
        close(wss->fd); 
        exit(1);
    } 
    //------------------------------------------------------------------------------ epoll init
     // 创建一个epoll句柄  
    int epoll_fd;  
    epoll_fd = epoll_create(EPOLL_RESPOND_NUM);  
    if(epoll_fd < 0)
    {  
        printf("server epoll_create failed\r\n"); 
        exit(1);
    }  
    
    int nfds;                     // epoll监听事件发生的个数  
    struct epoll_event ev;        // epoll事件结构体  
    struct epoll_event events[EPOLL_RESPOND_NUM];
    ev.events = EPOLLIN|EPOLLET;  // 	EPOLLIN		EPOLLET;    监听事件类型
    ev.data.fd = wss->fd;  
    // 向epoll注册server_sockfd监听事件  
    if(epoll_ctl(epoll_fd , EPOLL_CTL_ADD , wss->fd , &ev) < 0)
    {  
        printf("server epll_ctl : wss->fd register failed\r\n"); 
        close(epoll_fd);
        exit(1);
    }  
    
	//------------------------------------------------------------------------------ server receive
	printf("\r\n\r\n========== server start ! ==========\r\n\r\n");
    while(1)
    { 
    	// 等待事件发生  
        nfds = epoll_wait(epoll_fd , events , EPOLL_RESPOND_NUM , -1);  	// -1表示阻塞、其它数值为超时
        if(nfds < 0)
        {  
            printf("server start epoll_wait failed\r\n"); 
            close(epoll_fd);
            exit(1);
        }  
       
        for(j = 0 ; j < nfds ; j++)
        { 
        	//===================epoll错误 =================== 
            if ((events[j].events & EPOLLERR) || (events[j].events & EPOLLHUP))  
            {
            	    //printf("server epoll err !\r\n"); 
            	    //exit(1);
            	    printf("accept close : %d\r\n", events[j].data.fd);     // 与客户端连接出错, 主动断开当前 连接
					// 向epoll删除client_sockfd监听事件  
					//ev.events = EPOLLIN|EPOLLET;
        			ev.data.fd = events[j].data.fd; 
				    if(epoll_ctl(epoll_fd , EPOLL_CTL_DEL , events[j].data.fd , &ev) < 0) 
				    {  
				        printf("server epoll_ctl : EPOLL_CTL_DEL failed !\r\n"); 
				        close(epoll_fd);
				        exit(1);
				    }
				    arrayRemoveItem(wss->client_fd_array, EPOLL_RESPOND_NUM, events[j].data.fd);    // 从数组剔除fd
					close(events[j].data.fd);	//关闭通道
            }
        	//===================新通道接入事件===================
        	else if(events[j].data.fd == wss->fd)
        	{		
				//轮巡可能接入的新通道 并把通道号记录在accept_fd[]数组中
				accept_fd = accept(wss->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);  
				if(accept_fd >= 0)  //----------有新接入，通道号加1----------
				{ 	
					// 向epoll注册client_sockfd监听事件  
					//ev.events = EPOLLIN|EPOLLET;
		    		ev.data.fd = accept_fd; 
					if(epoll_ctl(epoll_fd , EPOLL_CTL_ADD , accept_fd , &ev) < 0) 
					{  
						 printf("server epoll_ctl : EPOLL_CTL_ADD failed !\r\n"); 
						 close(epoll_fd);
						 exit(1);
					}
					//send(accept_fd , "OK\r\n" , 4 , MSG_NOSIGNAL);
					printf("server fd/%d : accept\r\n", accept_fd);
					arrayAddItem(wss->client_fd_array, EPOLL_RESPOND_NUM, accept_fd);    // 添加fd到数组
				}
			}
			//===================接收数据事件===================
			else if(events[j].events & EPOLLIN)
			{		
				memset(wss->buf, 0, sizeof(wss->buf));
				ret = wss->callBack(events[j].data.fd , wss->buf , sizeof(wss->buf));
				if(ret <= 0)		//----------ret<=0时检查异常, 决定是否主动解除连接----------
				{
				    if(errno == EAGAIN || errno == EINTR)
                        ;
                    else
                    {
					    printf("accept close : %d\r\n", events[j].data.fd);
					    // 向epoll删除client_sockfd监听事件  
					    //ev.events = EPOLLIN|EPOLLET;
            			ev.data.fd = events[j].data.fd; 
				        if(epoll_ctl(epoll_fd , EPOLL_CTL_DEL , events[j].data.fd , &ev) < 0) 
				        {  
				            printf("server epoll_ctl : EPOLL_CTL_DEL failed !\r\n"); 
				            close(epoll_fd);
				            exit(1);
				        }
				        arrayRemoveItem(wss->client_fd_array, EPOLL_RESPOND_NUM, events[j].data.fd);    // 从数组剔除fd
					    close(events[j].data.fd);	//关闭通道
					}
				}
			}
			//===================发送数据事件===================
			else if(events[j].events & EPOLLOUT)
			    ;
		}
    }
	//关闭epoll句柄
    close(epoll_fd);
    //关闭socket
    close(wss->fd);
}

//
int server_callBack(int fd, char *buf, unsigned int bufLen)
{
    int ret;
    ret = webSocket_recv(fd , buf , bufLen, NULL);    // 使用websocket recv
    if(ret > 0)
	{
		printf("server(fd/%d) recv: %s\r\n", fd, buf);
		
		//===== 在这里根据客户端的请求内容, 提供相应的服务 =====
		if(strstr(buf, "hi~") != NULL)
		    ret = webSocket_send(fd, "Hi~ I am server", strlen("Hi~ I am server"), false, WDT_TXTDATA);
		else
            ;
		// ... ...
		// ...
	}
    else if(ret < 0)
    {
        if(strncmp(buf, "GET", 3) == 0)	//握手,建立连接
            ret = webSocket_serverLinkToClient(fd, buf, ret);
    }
	return ret;
}


int main(void)
{
    int exitFlag;
    int i, client_fd;
    pthread_t sever_thread_id;
    WebSocket_Server wss;
    
    //===== 初始化服务器参数 =====
    memset(&wss, 0, sizeof(wss));
    //strcpy(wss.ip, "127.0.0.1");     
    wss.port = 9999;
    wss.callBack = &server_callBack; // 响应客户端时, 需要干嘛?
    
    //===== 开辟线程, 管理服务器 =====
    if(pthread_create(&sever_thread_id, NULL, (void*)&server_thread_fun, (void *)(&wss)) != 0)
	{
		printf("create server false !\r\n");
		exit(1);
	} 
	//
    exitFlag = 0;
	while(!exitFlag)
	{
        // 每3秒推送信息给所有客户端
	    for(i = 0; i < EPOLL_RESPOND_NUM; i++)
	    {
	        if(wss.client_fd_array[i][1] != 0 && wss.client_fd_array[i][0] > 0)
	        {
	            client_fd = wss.client_fd_array[i][0];
	            if(webSocket_send(client_fd, "======== 这是推送 ========", strlen("======== 这是推送 ========"), false, WDT_TXTDATA) < 0)
                {
                    printf("server webSocket_send err !!\r\n");
                    exitFlag = 1;
                    break;
                }
	        }
	    }
	    webSocket_delayms(3000);
	}
	
	//==============================
    pthread_cancel(sever_thread_id);     // 等待线程关闭
    printf("server close !\r\n");
    return 0;
}







