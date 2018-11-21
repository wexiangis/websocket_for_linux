
#include "websocket_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>  // epoll管理服务器的连接和接收触发
#include <sys/socket.h>
#include <netinet/ip.h>
#include <pthread.h>    // 使用多线程

#define		EPOLL_RESPOND_NUM		100		// epoll同时响应事件数量

typedef int (*CallBackFun)(int fd, char *buf, unsigned int bufLen);

typedef struct{
    int fd;
    int client_fd_array[EPOLL_RESPOND_NUM][2];
    char ip[24];
    int port;
    char buf[10240];
    CallBackFun action;
}Websocket_Server;

//////////////////////////////////////////////////////////// Tool Function ///////////////////////////////////////////////////////////////////////////////

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

//////////////////////////////////////////////////////////// Call Back Function ///////////////////////////////////////////////////////////////////////////////

int call(CallBackFun function, int fd, char *buf, unsigned int bufLen)
{
    return function(fd, buf, bufLen);
}

//////////////////////////////////////////////////////////// Server Function ///////////////////////////////////////////////////////////////////////////////

void server_thread_fun(void *arge)
{
	int ret , i , j;
	int accept_fd;
	int socAddrLen;
	struct sockaddr_in acceptAddr;
	struct sockaddr_in serverAddr;
	//
	Websocket_Server *ws = (Websocket_Server *)arge;
	//
	memset(&serverAddr , 0 , sizeof(serverAddr)); 			// 数据初始化--清零     
    serverAddr.sin_family = AF_INET; 									// 设置为IP通信     
    //serverAddr.sin_addr.s_addr = inet_addr(ws->ip);		// 服务器IP地址
    serverAddr.sin_addr.s_addr = INADDR_ANY;		// 服务器IP地址
    serverAddr.sin_port = htons(ws->port); 						// 服务器端口号    
	//
	socAddrLen = sizeof(struct sockaddr_in);
	
	//------------------------------------------------------------------------------ socket init
	//socket init
	ws->fd = socket(AF_INET, SOCK_STREAM,0);  
    if(ws->fd <= 0)  
    {  
        printf("server cannot create socket !\r\n"); 
        exit(1);
    }    
    
    //设置为非阻塞接收
    ret = fcntl(ws->fd , F_GETFL , 0);
    fcntl(ws->fd , F_SETFL , ret | O_NONBLOCK);
    
    //bind sockfd & addr  
    while(bind(ws->fd, (struct sockaddr *)&serverAddr, sizeof(struct sockaddr)) < 0 )
        webSocket_delayms(1);
    
    //listen sockfd   
    ret = listen(ws->fd, 0);  
    if(ret < 0) 
    {  
        printf("server cannot listen request\r\n"); 
        close(ws->fd); 
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
    
    int nfds;                                  // epoll监听事件发生的个数  
    struct epoll_event ev;            // epoll事件结构体  
    struct epoll_event events[EPOLL_RESPOND_NUM];
    ev.events = EPOLLIN|EPOLLET;  			// 	EPOLLIN		EPOLLET;    监听事件类型
    ev.data.fd = ws->fd;  
    // 向epoll注册server_sockfd监听事件  
    if(epoll_ctl(epoll_fd , EPOLL_CTL_ADD , ws->fd , &ev) < 0)
    {  
        printf("server epll_ctl : ws->fd register failed\r\n"); 
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
				    arrayRemoveItem(ws->client_fd_array, EPOLL_RESPOND_NUM, events[j].data.fd);    // 从数组剔除fd
					close(events[j].data.fd);	//关闭通道
            }
        	//===================新通道接入事件===================
        	else if(events[j].data.fd == ws->fd)
        	{		
				//轮巡可能接入的新通道 并把通道号记录在accept_fd[]数组中
				accept_fd = accept(ws->fd, (struct sockaddr *)&acceptAddr, &socAddrLen);  
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
					arrayAddItem(ws->client_fd_array, EPOLL_RESPOND_NUM, accept_fd);    // 添加fd到数组
				}
			}
			//===================接收数据事件===================
			else if(events[j].events & EPOLLIN)
			{		
				//ret = recv(events[j].data.fd , ws->buf , sizeof(ws->buf) ,  MSG_NOSIGNAL); 			//  MSG_NOSIGNAL(非阻塞)  MSG_DONTWAIT  MSG_WAITALL
				memset(ws->buf, 0, sizeof(ws->buf));
				ret = call(ws->action, events[j].data.fd , ws->buf , sizeof(ws->buf));
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
				        arrayRemoveItem(ws->client_fd_array, EPOLL_RESPOND_NUM, events[j].data.fd);    // 从数组剔除fd
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
    close(ws->fd); 
    //退出线程
    pthread_exit(NULL); 
}
//////////////////////////////////////////////////////////// Call Back Function ///////////////////////////////////////////////////////////////////////////////

int server_action(int fd, char *buf, unsigned int bufLen)
{
    int ret;
    ret = webSocket_recv(fd , buf , bufLen);    // 使用websocket recv
    if(ret > 0)
	{
		printf("server fd/%d : len/%d %s\r\n", fd, ret, buf);
		
		//===== 在这里根据客户端的请求内容, 提供相应的服务 =====
		if(strstr(buf, "connect") != NULL)     // 成功连上之后, 发个测试数据
		    ret = webSocket_send(fd, "Hello !", strlen("Hello !"), false, WDT_TXTDATA);
		else if(strstr(buf, "Hello") != NULL)
		    ret = webSocket_send(fd, "I am Server_Test", strlen("I am Server_Test"), false, WDT_TXTDATA);
		else
		    ret = webSocket_send(fd, "You are carefree ...", strlen("You are carefree ..."), false, WDT_TXTDATA);
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

//////////////////////////////////////////////////////////// Main Function ///////////////////////////////////////////////////////////////////////////////

int main(void)
{
    int ret;
    int i, client_fd;
    pthread_t sever_thread_id;
    Websocket_Server ws;
    
    //===== 初始化服务器参数 =====
    memset(&ws, 0, sizeof(ws));
    //strcpy(ws.ip, "127.0.0.1");     
    ws.port = 9999;
    ws.action = &server_action;     // 响应客户端时, 需要干嘛?
    
    //===== 开辟线程, 管理服务器 =====
    ret = pthread_create(&sever_thread_id, NULL, (void*)&server_thread_fun, (void *)(&ws));  // 传递参数到线程
	if(ret != 0)
	{
		printf("create server false !\r\n");
		exit(1);
	} 
	
	while(1)
	{
	    for(i = 0; i < EPOLL_RESPOND_NUM; i++)
	    {
	        if(ws.client_fd_array[i][1] != 0 && ws.client_fd_array[i][0] > 0)
	        {
	            client_fd = ws.client_fd_array[i][0];
	            /////////////////////////////////////////////////////////////////////////////////////////////   服务器可以在这里对所有已连入的客户端 推送点垃圾广告
	            
	            ret = webSocket_send(client_fd, "\\O^O/  <-.<-  TAT  =.=#  -.-! ...", strlen("\\O^O/  <-.<-  TAT  =.=#  -.-! ..."), false, WDT_TXTDATA);
	            
	            /////////////////////////////////////////////////////////////////////////////////////////////
	        }
	    }
	    webSocket_delayms(5000);
	}
	
	//==============================
    pthread_join(sever_thread_id, NULL);     // 等待线程关闭
    printf("server close !\r\n");
    return 0;
}







