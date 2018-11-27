
#include "websocket_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>     // 使用 malloc, calloc等动态分配内存方法
#include <time.h>       // 获取系统时间
#include <errno.h>
#include <fcntl.h>      // 非阻塞
#include <sys/un.h> 
#include <arpa/inet.h>  // inet_addr()
#include <unistd.h>     // close()
#include <sys/types.h>  // 文件IO操作
#include <sys/socket.h> //
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>      // gethostbyname, gethostbyname2, gethostbyname_r, gethostbyname_r2
#include <sys/un.h> 
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>  // SIOCSIFADDR

//==============================================================================================
//======================================== 设置和工具部分 =======================================
//==============================================================================================

// 连接服务器
#define WEBSOCKET_LOGIN_CONNECT_TIMEOUT  1000            // 登录连接超时设置 1000ms
#define WEBSOCKET_LOGIN_RESPOND_TIMEOUT  (1000 + WEBSOCKET_LOGIN_CONNECT_TIMEOUT) // 登录等待回应超时设置 1000ms
// 发收
// 生成握手key的长度
#define WEBSOCKET_SHAKE_KEY_LEN     16

//==================== delay ms ====================
void webSocket_delayms(unsigned int ms)
{
    struct timeval tim;
    tim.tv_sec = ms/1000;
    tim.tv_usec = (ms%1000)*1000;
    select(0, NULL, NULL, NULL, &tim);
}

//-------------------- IP控制 --------------------

int netCheck_setIP(char *devName, char *ip)
{
    struct ifreq temp;
    struct sockaddr_in *addr;
    int fd, ret;
    //
    strcpy(temp.ifr_name, devName);
    if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
      return -1;
    //
    addr = (struct sockaddr_in *)&(temp.ifr_addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = inet_addr(ip);
    ret = ioctl(fd, SIOCSIFADDR, &temp);
    //
    close(fd);
    if(ret < 0)
       return -1;
    return 0;
}

void netCheck_getIP(char *devName, char *ip)
{
    struct ifreq temp;
    struct sockaddr_in *addr;
    int fd, ret;
    //
    strcpy(temp.ifr_name, devName);
    if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return;
    ret = ioctl(fd, SIOCGIFADDR, &temp);
    close(fd);
    if(ret < 0)
        return;
    //
    addr = (struct sockaddr_in *)&(temp.ifr_addr);
    strcpy(ip, inet_ntoa(addr->sin_addr));
    //
    // return ip;
}

//==================== 域名转IP ====================

typedef struct{
    pthread_t thread_id;
    char ip[256];
    bool result;
    bool actionEnd;
}GetHostName_Struct;
//
void *websocket_getHost_fun(void *arge)
{
    int ret;
    //int i;
    char buf[1024];
    struct hostent host_body, *host = NULL;
    struct in_addr **addr_list;
    GetHostName_Struct *gs = (GetHostName_Struct *)arge;

    /*  此类方法不可重入!  即使关闭线程
    if((host = gethostbyname(gs->ip)) == NULL)
    //if((host = gethostbyname2(gs->ip, AF_INET)) == NULL)
    {
        gs->actionEnd = true;
        return NULL;
    }*/
    if(gethostbyname_r(gs->ip, &host_body, buf, sizeof(buf), &host, &ret))
    {
        gs->actionEnd = true;
        return NULL;
    }
    if(host == NULL)
    {
        gs->actionEnd = true;
        return NULL;
    }
    addr_list = (struct in_addr **)host->h_addr_list;
    //printf("ip name : %s\r\nip list : ", host->h_name);
    //for(i = 0; addr_list[i] != NULL; i++) printf("%s, ", inet_ntoa(*addr_list[i])); printf("\r\n");
    if(addr_list[0] == NULL)
    {
        gs->actionEnd = true;
        return NULL;
    }
    memset(gs->ip, 0, sizeof(gs->ip));
    strcpy(gs->ip, (char *)(inet_ntoa(*addr_list[0])));
    gs->result = true;
    gs->actionEnd = true;
    return NULL;
}
//
int websocket_getIpByHostName(char *hostName, char *backIp)
{
    int i, timeOut = 1;
     GetHostName_Struct gs;
    if(hostName == NULL)
        return -1;
    else if(strlen(hostName) < 1)
        return -1;
    //----- 开线程从域名获取IP -----
    memset(&gs, 0, sizeof(GetHostName_Struct));
    strcpy(gs.ip, hostName);
    gs.result = false;
    gs.actionEnd = false;
    if (pthread_create(&gs.thread_id, NULL, (void *)websocket_getHost_fun, &gs) < 0)
        return -1;
    i = 0;
    while(!gs.actionEnd)
    {
        if(++i > 10)
        {
            i = 0;
            if(++timeOut > 1000)
                break;
        }
        webSocket_delayms(1000);// 1ms延时
    }
    // pthread_cancel(gs.thread_id);
    pthread_join(gs.thread_id, NULL);
    if(!gs.result)
        return -timeOut;
    //----- 开线程从域名获取IP -----
    memset(backIp, 0, strlen(backIp));
    strcpy(backIp, gs.ip);
    return timeOut;
}

//==================== 加密方法BASE64 ====================

//base64编/解码用的基础字符集
const char websocket_base64char[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*******************************************************************************
 * 名称: websocket_base64_encode
 * 功能: ascii编码为base64格式
 * 形参: bindata : ascii字符串输入
 *            base64 : base64字符串输出
 *          binlength : bindata的长度
 * 返回: base64字符串长度
 * 说明: 无
 ******************************************************************************/
int websocket_base64_encode( const unsigned char *bindata, char *base64, int binlength)
{
    int i, j;
    unsigned char current;
    for ( i = 0, j = 0 ; i < binlength ; i += 3 )
    {
        current = (bindata[i] >> 2) ;
        current &= (unsigned char)0x3F;
        base64[j++] = websocket_base64char[(int)current];
        current = ( (unsigned char)(bindata[i] << 4 ) ) & ( (unsigned char)0x30 ) ;
        if ( i + 1 >= binlength )
        {
            base64[j++] = websocket_base64char[(int)current];
            base64[j++] = '=';
            base64[j++] = '=';
            break;
        }
        current |= ( (unsigned char)(bindata[i+1] >> 4) ) & ( (unsigned char) 0x0F );
        base64[j++] = websocket_base64char[(int)current];
        current = ( (unsigned char)(bindata[i+1] << 2) ) & ( (unsigned char)0x3C ) ;
        if ( i + 2 >= binlength )
        {
            base64[j++] = websocket_base64char[(int)current];
            base64[j++] = '=';
            break;
        }
        current |= ( (unsigned char)(bindata[i+2] >> 6) ) & ( (unsigned char) 0x03 );
        base64[j++] = websocket_base64char[(int)current];
        current = ( (unsigned char)bindata[i+2] ) & ( (unsigned char)0x3F ) ;
        base64[j++] = websocket_base64char[(int)current];
    }
    base64[j] = '\0';
    return j;
}
/*******************************************************************************
 * 名称: websocket_base64_decode
 * 功能: base64格式解码为ascii
 * 形参: base64 : base64字符串输入
 *            bindata : ascii字符串输出
 * 返回: 解码出来的ascii字符串长度
 * 说明: 无
 ******************************************************************************/
int websocket_base64_decode( const char *base64, unsigned char *bindata)
{
    int i, j;
    unsigned char k;
    unsigned char temp[4];
    for ( i = 0, j = 0; base64[i] != '\0' ; i += 4 )
    {
        memset( temp, 0xFF, sizeof(temp) );
        for ( k = 0 ; k < 64 ; k ++ )
        {
            if ( websocket_base64char[k] == base64[i] )
                temp[0]= k;
        }
        for ( k = 0 ; k < 64 ; k ++ )
        {
            if ( websocket_base64char[k] == base64[i+1] )
                temp[1]= k;
        }
        for ( k = 0 ; k < 64 ; k ++ )
        {
            if ( websocket_base64char[k] == base64[i+2] )
                temp[2]= k;
        }
        for ( k = 0 ; k < 64 ; k ++ )
        {
            if ( websocket_base64char[k] == base64[i+3] )
                temp[3]= k;
        }
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[0] << 2))&0xFC)) | \
                ((unsigned char)((unsigned char)(temp[1]>>4)&0x03));
        if ( base64[i+2] == '=' )
            break;
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[1] << 4))&0xF0)) | \
                ((unsigned char)((unsigned char)(temp[2]>>2)&0x0F));
        if ( base64[i+3] == '=' )
            break;
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[2] << 6))&0xF0)) | \
                ((unsigned char)(temp[3]&0x3F));
    }
    return j;
}

//==================== 加密方法 sha1哈希 ====================

typedef struct SHA1Context{  
    unsigned Message_Digest[5];        
    unsigned Length_Low;               
    unsigned Length_High;              
    unsigned char Message_Block[64];   
    int Message_Block_Index;           
    int Computed;                      
    int Corrupted;                     
} SHA1Context;  

#define SHA1CircularShift(bits,word) ((((word) << (bits)) & 0xFFFFFFFF) | ((word) >> (32-(bits))))  

void SHA1ProcessMessageBlock(SHA1Context *context)
{  
    const unsigned K[] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };  
    int         t;                  
    unsigned    temp;               
    unsigned    W[80];              
    unsigned    A, B, C, D, E;      
  
    for(t = 0; t < 16; t++) 
    {  
        W[t] = ((unsigned) context->Message_Block[t * 4]) << 24;  
        W[t] |= ((unsigned) context->Message_Block[t * 4 + 1]) << 16;  
        W[t] |= ((unsigned) context->Message_Block[t * 4 + 2]) << 8;  
        W[t] |= ((unsigned) context->Message_Block[t * 4 + 3]);  
    }  
      
    for(t = 16; t < 80; t++)  
        W[t] = SHA1CircularShift(1,W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]);  
  
    A = context->Message_Digest[0];  
    B = context->Message_Digest[1];  
    C = context->Message_Digest[2];  
    D = context->Message_Digest[3];  
    E = context->Message_Digest[4];  
  
    for(t = 0; t < 20; t++) 
    {  
        temp =  SHA1CircularShift(5,A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];  
        temp &= 0xFFFFFFFF;  
        E = D;  
        D = C;  
        C = SHA1CircularShift(30,B);  
        B = A;  
        A = temp;  
    }  
    for(t = 20; t < 40; t++) 
    {  
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[1];  
        temp &= 0xFFFFFFFF;  
        E = D;  
        D = C;  
        C = SHA1CircularShift(30,B);  
        B = A;  
        A = temp;  
    }  
    for(t = 40; t < 60; t++) 
    {  
        temp = SHA1CircularShift(5,A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];  
        temp &= 0xFFFFFFFF;  
        E = D;  
        D = C;  
        C = SHA1CircularShift(30,B);  
        B = A;  
        A = temp;  
    }  
    for(t = 60; t < 80; t++) 
    {  
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[3];  
        temp &= 0xFFFFFFFF;  
        E = D;  
        D = C;  
        C = SHA1CircularShift(30,B);  
        B = A;  
        A = temp;  
    }  
    context->Message_Digest[0] = (context->Message_Digest[0] + A) & 0xFFFFFFFF;  
    context->Message_Digest[1] = (context->Message_Digest[1] + B) & 0xFFFFFFFF;  
    context->Message_Digest[2] = (context->Message_Digest[2] + C) & 0xFFFFFFFF;  
    context->Message_Digest[3] = (context->Message_Digest[3] + D) & 0xFFFFFFFF;  
    context->Message_Digest[4] = (context->Message_Digest[4] + E) & 0xFFFFFFFF;  
    context->Message_Block_Index = 0;  
} 

void SHA1Reset(SHA1Context *context)
{
    context->Length_Low             = 0;  
    context->Length_High            = 0;  
    context->Message_Block_Index    = 0;  
  
    context->Message_Digest[0]      = 0x67452301;  
    context->Message_Digest[1]      = 0xEFCDAB89;  
    context->Message_Digest[2]      = 0x98BADCFE;  
    context->Message_Digest[3]      = 0x10325476;  
    context->Message_Digest[4]      = 0xC3D2E1F0;  
  
    context->Computed   = 0;  
    context->Corrupted  = 0;  
}  
  
void SHA1PadMessage(SHA1Context *context)
{  
    if (context->Message_Block_Index > 55) 
    {  
        context->Message_Block[context->Message_Block_Index++] = 0x80;  
        while(context->Message_Block_Index < 64)  context->Message_Block[context->Message_Block_Index++] = 0;  
        SHA1ProcessMessageBlock(context);  
        while(context->Message_Block_Index < 56) context->Message_Block[context->Message_Block_Index++] = 0;  
    } 
    else 
    {  
        context->Message_Block[context->Message_Block_Index++] = 0x80;  
        while(context->Message_Block_Index < 56) context->Message_Block[context->Message_Block_Index++] = 0;  
    }  
    context->Message_Block[56] = (context->Length_High >> 24 ) & 0xFF;  
    context->Message_Block[57] = (context->Length_High >> 16 ) & 0xFF;  
    context->Message_Block[58] = (context->Length_High >> 8 ) & 0xFF;  
    context->Message_Block[59] = (context->Length_High) & 0xFF;  
    context->Message_Block[60] = (context->Length_Low >> 24 ) & 0xFF;  
    context->Message_Block[61] = (context->Length_Low >> 16 ) & 0xFF;  
    context->Message_Block[62] = (context->Length_Low >> 8 ) & 0xFF;  
    context->Message_Block[63] = (context->Length_Low) & 0xFF;  
  
    SHA1ProcessMessageBlock(context);  
} 

int SHA1Result(SHA1Context *context)
{
    if (context->Corrupted) 
    {  
        return 0;  
    }  
    if (!context->Computed) 
    {  
        SHA1PadMessage(context);  
        context->Computed = 1;  
    }  
    return 1;  
}  
  
  
void SHA1Input(SHA1Context *context,const char *message_array,unsigned length){  
    if (!length) 
        return;  
  
    if (context->Computed || context->Corrupted)
    {  
        context->Corrupted = 1;  
        return;  
    }  
  
    while(length-- && !context->Corrupted)
    {  
        context->Message_Block[context->Message_Block_Index++] = (*message_array & 0xFF);  
  
        context->Length_Low += 8;  
  
        context->Length_Low &= 0xFFFFFFFF;  
        if (context->Length_Low == 0)
        {  
            context->Length_High++;  
            context->Length_High &= 0xFFFFFFFF;  
            if (context->Length_High == 0) context->Corrupted = 1;  
        }  
  
        if (context->Message_Block_Index == 64)
        {  
            SHA1ProcessMessageBlock(context);  
        }  
        message_array++;  
    }  
}

/* 
int sha1_hash(const char *source, char *lrvar){// Main 
    SHA1Context sha; 
    char buf[128]; 
 
    SHA1Reset(&sha); 
    SHA1Input(&sha, source, strlen(source)); 
 
    if (!SHA1Result(&sha)){ 
        printf("SHA1 ERROR: Could not compute message digest"); 
        return -1; 
    } else { 
        memset(buf,0,sizeof(buf)); 
        sprintf(buf, "%08X%08X%08X%08X%08X", sha.Message_Digest[0],sha.Message_Digest[1], 
        sha.Message_Digest[2],sha.Message_Digest[3],sha.Message_Digest[4]); 
        //lr_save_string(buf, lrvar); 
         
        return strlen(buf); 
    } 
} 
*/  
char * sha1_hash(const char *source){   // Main  
    SHA1Context sha;  
    char *buf;//[128];  
  
    SHA1Reset(&sha);  
    SHA1Input(&sha, source, strlen(source));  
  
    if (!SHA1Result(&sha))
    {  
        printf("SHA1 ERROR: Could not compute message digest");  
        return NULL;  
    } 
    else 
    {  
        buf = (char *)malloc(128);  
        memset(buf, 0, 128);  
        sprintf(buf, "%08X%08X%08X%08X%08X", sha.Message_Digest[0],sha.Message_Digest[1],  
        sha.Message_Digest[2],sha.Message_Digest[3],sha.Message_Digest[4]);  
        //lr_save_string(buf, lrvar);  
          
        //return strlen(buf);  
        return buf;  
    }  
}  

int tolower(int c)   
{   
    if (c >= 'A' && c <= 'Z')   
    {   
        return c + 'a' - 'A';   
    }   
    else   
    {   
        return c;   
    }   
}

int htoi(const char s[], int start, int len)   
{   
    int i, j;   
    int n = 0;   
    if (s[0] == '0' && (s[1]=='x' || s[1]=='X')) //判断是否有前导0x或者0X  
    {   
        i = 2;   
    }   
    else   
    {   
        i = 0;   
    }   
    i+=start;  
    j=0;  
    for (; (s[i] >= '0' && s[i] <= '9')   
       || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >='A' && s[i] <= 'F');++i)   
    {     
        if(j>=len)  
        {  
            break;  
        }  
        if (tolower(s[i]) > '9')   
        {   
            n = 16 * n + (10 + tolower(s[i]) - 'a');   
        }   
        else   
        {   
            n = 16 * n + (tolower(s[i]) - '0');   
        }   
        j++;  
    }   
    return n;   
}   

//==============================================================================================
//======================================== websocket部分 =======================================
//==============================================================================================

// websocket根据data[0]判别数据包类型
// typedef enum{
//     WDT_MINDATA = -20,      // 0x0：标识一个中间数据包
//     WDT_TXTDATA = -19,      // 0x1：标识一个text类型数据包
//     WDT_BINDATA = -18,      // 0x2：标识一个binary类型数据包
//     WDT_DISCONN = -17,      // 0x8：标识一个断开连接类型数据包
//     WDT_PING = -16,     // 0x8：标识一个断开连接类型数据包
//     WDT_PONG = -15,     // 0xA：表示一个pong类型数据包
//     WDT_ERR = -1,
//     WDT_NULL = 0
// }WebsocketData_Type;
/*******************************************************************************
 * 名称: webSocket_getRandomString
 * 功能: 生成随机字符串
 * 形参: *buf：随机字符串存储到
 *              len : 生成随机字符串长度
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void webSocket_getRandomString(unsigned char *buf, unsigned int len)
{
    unsigned int i;
    unsigned char temp;
    srand((int)time(0));
    for(i = 0; i < len; i++)
    {
        temp = (unsigned char)(rand()%256);
        if(temp == 0)   // 随机数不要0, 0 会干扰对字符串长度的判断
            temp = 128;
        buf[i] = temp;
    }
}
/*******************************************************************************
 * 名称: webSocket_buildShakeKey
 * 功能: client端使用随机数构建握手用的key
 * 形参: *key：随机生成的握手key
 * 返回: key的长度
 * 说明: 无
 ******************************************************************************/
int webSocket_buildShakeKey(unsigned char *key)
{
    unsigned char tempKey[WEBSOCKET_SHAKE_KEY_LEN] = {0};
    webSocket_getRandomString(tempKey, WEBSOCKET_SHAKE_KEY_LEN);
    return websocket_base64_encode((const unsigned char *)tempKey, (char *)key, WEBSOCKET_SHAKE_KEY_LEN);
}
/*******************************************************************************
 * 名称: webSocket_buildRespondShakeKey
 * 功能: server端在接收client端的key后,构建回应用的key
 * 形参: *acceptKey：来自客户端的key字符串
 *         acceptKeyLen : 长度
 *          *respondKey :  在 acceptKey 之后加上 GUID, 再sha1哈希, 再转成base64得到 respondKey
 * 返回: respondKey的长度(肯定比acceptKey要长)
 * 说明: 无
 ******************************************************************************/
int webSocket_buildRespondShakeKey(unsigned char *acceptKey, unsigned int acceptKeyLen, unsigned char *respondKey)
{
    char *clientKey;  
    char *sha1DataTemp;  
    char *sha1Data;  
    int i, n;  
    const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";  
    unsigned int GUIDLEN;
    
    if(acceptKey == NULL)  
        return 0;  
    GUIDLEN = sizeof(GUID);
    clientKey = (char *)calloc(acceptKeyLen + GUIDLEN + 10, sizeof(char));  
    memset(clientKey, 0, (acceptKeyLen + GUIDLEN + 10));
    //
    memcpy(clientKey, acceptKey, acceptKeyLen); 
    memcpy(&clientKey[acceptKeyLen], GUID, GUIDLEN);
    clientKey[acceptKeyLen + GUIDLEN] = '\0';
    //
    sha1DataTemp = sha1_hash(clientKey);  
    n = strlen((const char *)sha1DataTemp);  
    sha1Data = (char *)calloc(n / 2 + 1, sizeof(char));  
    memset(sha1Data, 0, n / 2 + 1);  
   //
    for(i = 0; i < n; i += 2)  
        sha1Data[ i / 2 ] = htoi(sha1DataTemp, i, 2);      
    n = websocket_base64_encode((const unsigned char *)sha1Data, (char *)respondKey, (n / 2));
    //
    free(sha1DataTemp);
    free(sha1Data);
    free(clientKey);
    return n;
}
/*******************************************************************************
 * 名称: webSocket_matchShakeKey
 * 功能: client端收到来自服务器回应的key后进行匹配,以验证握手成功
 * 形参: *myKey：client端请求握手时发给服务器的key
 *            myKeyLen : 长度
 *          *acceptKey : 服务器回应的key
 *           acceptKeyLen : 长度
 * 返回: 0 成功  -1 失败
 * 说明: 无
 ******************************************************************************/
int webSocket_matchShakeKey(unsigned char *myKey, unsigned int myKeyLen, unsigned char *acceptKey, unsigned int acceptKeyLen)
{
    int retLen;
    unsigned char tempKey[256] = {0};
    //
    retLen = webSocket_buildRespondShakeKey(myKey, myKeyLen, tempKey);
    //printf("webSocket_matchShakeKey :\r\n%d : %s\r\n%d : %s\r\n", acceptKeyLen, acceptKey, retLen, tempKey);
    //
    if(retLen != acceptKeyLen)
    {
        printf("webSocket_matchShakeKey : len err\r\n%s\r\n%s\r\n%s\r\n", myKey, tempKey, acceptKey);
        return -1;
    }
    else if(strcmp((const char *)tempKey, (const char *)acceptKey) != 0)
    {
        printf("webSocket_matchShakeKey : str err\r\n%s\r\n%s\r\n", tempKey, acceptKey);
        return -1;
    }
    return 0;
}
/*******************************************************************************
 * 名称: webSocket_buildHttpHead
 * 功能: 构建client端连接服务器时的http协议头, 注意websocket是GET形式的
 * 形参: *ip：要连接的服务器ip字符串
 *          port : 服务器端口
 *    *interfacePath : 要连接的端口地址
 *      *shakeKey : 握手key, 可以由任意的16位字符串打包成base64后得到
 *      *package : 存储最后打包好的内容
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void webSocket_buildHttpHead(char *ip, int port, char *interfacePath, unsigned char *shakeKey, char *package)
{
    const char httpDemo[] = "GET %s HTTP/1.1\r\n"
                            "Connection: Upgrade\r\n"
                            "Host: %s:%d\r\n"
                            "Sec-WebSocket-Key: %s\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Upgrade: websocket\r\n\r\n";
    sprintf(package, httpDemo, interfacePath, ip, port, shakeKey);
}
/*******************************************************************************
 * 名称: webSocket_buildHttpRespond
 * 功能: 构建server端回复client连接请求的http协议
 * 形参: *acceptKey：来自client的握手key
 *          acceptKeyLen : 长度
 *          *package : 存储
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void webSocket_buildHttpRespond(unsigned char *acceptKey, unsigned int acceptKeyLen, char *package)
{
    const char httpDemo[] = "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Server: Microsoft-HTTPAPI/2.0\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n"
                            "%s\r\n\r\n";  // 时间打包待续        // 格式如 "Date: Tue, 20 Jun 2017 08:50:41 CST\r\n"
    time_t now;
    struct tm *tm_now;
    char timeStr[256] = {0};
    unsigned char respondShakeKey[256] = {0};
    // 构建回应的握手key
    webSocket_buildRespondShakeKey(acceptKey, acceptKeyLen, respondShakeKey);   
    // 构建回应时间字符串
    time(&now);
    tm_now = localtime(&now);
    strftime(timeStr, sizeof(timeStr), "Date: %a, %d %b %Y %T %Z", tm_now);
    // 组成回复信息
    sprintf(package, httpDemo, respondShakeKey, timeStr);
}
/*******************************************************************************
 * 名称: webSocket_enPackage
 * 功能: websocket数据收发阶段的数据打包, 通常client发server的数据都要isMask(掩码)处理, 反之server到client却不用
 * 形参: *data：准备发出的数据
 *          dataLen : 长度
 *        *package : 打包后存储地址
 *        packageMaxLen : 存储地址可用长度
 *          isMask : 是否使用掩码     1要   0 不要
 *          type : 数据类型, 由打包后第一个字节决定, 这里默认是数据传输, 即0x81
 * 返回: 打包后的长度(会比原数据长2~16个字节不等)      <=0 打包失败 
 * 说明: 无
 ******************************************************************************/
int webSocket_enPackage(unsigned char *data, unsigned int dataLen, unsigned char *package, unsigned int packageMaxLen, bool isMask, WebsocketData_Type type)
{
    unsigned char maskKey[4] = {0};    // 掩码
    unsigned char temp1, temp2;
    int count;
    unsigned int i, len = 0;
    
    if(packageMaxLen < 2)
        return -1;
    
    if(type == WDT_MINDATA)
        *package++ = 0x00;
    else if(type == WDT_TXTDATA)
        *package++ = 0x81;
    else if(type == WDT_BINDATA)
        *package++ = 0x82;
    else if(type == WDT_DISCONN)
        *package++ = 0x88;
    else if(type == WDT_PING)
        *package++ = 0x89;
    else if(type == WDT_PONG)
        *package++ = 0x8A;
    else
        return -1;
    //
    if(isMask)
        *package = 0x80;
    len += 1;
    //
    if(dataLen < 126)
    {
        *package++ |= (dataLen&0x7F);
        len += 1;
    }
    else if(dataLen < 65536)
    {
        if(packageMaxLen < 4)
            return -1;
        *package++ |= 0x7E;
        *package++ = (char)((dataLen >> 8) & 0xFF);
        *package++ = (unsigned char)((dataLen >> 0) & 0xFF);
        len += 3;
    }
    else if(dataLen < 0xFFFFFFFF)
    {
        if(packageMaxLen < 10)
            return -1;
        *package++ |= 0x7F;
        *package++ = 0; //(char)((dataLen >> 56) & 0xFF);   // 数据长度变量是 unsigned int dataLen, 暂时没有那么多数据
        *package++ = 0; //(char)((dataLen >> 48) & 0xFF);
        *package++ = 0; //(char)((dataLen >> 40) & 0xFF);
        *package++ = 0; //(char)((dataLen >> 32) & 0xFF);
        *package++ = (char)((dataLen >> 24) & 0xFF);        // 到这里就够传4GB数据了
        *package++ = (char)((dataLen >> 16) & 0xFF);
        *package++ = (char)((dataLen >> 8) & 0xFF);
        *package++ = (char)((dataLen >> 0) & 0xFF);
        len += 9;
    }
    //
    if(isMask)    // 数据使用掩码时, 使用异或解码, maskKey[4]依次和数据异或运算, 逻辑如下
    {
        if(packageMaxLen < len + dataLen + 4)
            return -1;
        webSocket_getRandomString(maskKey, sizeof(maskKey));    // 随机生成掩码
        *package++ = maskKey[0];
        *package++ = maskKey[1];
        *package++ = maskKey[2];
        *package++ = maskKey[3];
        len += 4;
        for(i = 0, count = 0; i < dataLen; i++)
        {
            temp1 = maskKey[count];
            temp2 = data[i];
            *package++ = (char)(((~temp1)&temp2) | (temp1&(~temp2)));  // 异或运算后得到数据
            count += 1;
            if(count >= sizeof(maskKey))    // maskKey[4]循环使用
                count = 0;
        }
        len += i;
        *package = '\0';
    }
    else    // 数据没使用掩码, 直接复制数据段
    {
        if(packageMaxLen < len + dataLen)
            return -1;
        memcpy(package, data, dataLen);
        package[dataLen] = '\0';
        len += dataLen;
    }
    //
    return len;
}
/*******************************************************************************
 * 名称: webSocket_dePackage
 * 功能: websocket数据收发阶段的数据解包, 通常client发server的数据都要isMask(掩码)处理, 反之server到client却不用
 * 形参: *data：解包的数据
 *      dataLen : 长度
 *      *package : 解包后存储地址
 *      packageMaxLen : 存储地址可用长度
 *      *packageLen : 解包所得长度
 * 返回: 解包识别的数据类型 如 : txt数据, bin数据, ping, pong等
 * 说明: 无
 ******************************************************************************/
int webSocket_dePackage(unsigned char *data, unsigned int dataLen, unsigned char *package, unsigned int packageMaxLen, unsigned int *packageLen, unsigned int *packageHeadLen)
{
    unsigned char maskKey[4] = {0};    // 掩码
    unsigned char temp1, temp2;
    char Mask = 0, type;
    int count, ret;
    unsigned int i, len = 0, dataStart = 2;
    if(dataLen < 2)
        return WDT_ERR;
    
    type = data[0]&0x0F;
    
    if((data[0]&0x80) == 0x80)
    {
        if(type == 0x01) 
            ret = WDT_TXTDATA;
        else if(type == 0x02) 
            ret = WDT_BINDATA;
        else if(type == 0x08) 
            ret = WDT_DISCONN;
        else if(type == 0x09) 
            ret = WDT_PING;
        else if(type == 0x0A) 
            ret = WDT_PONG;
        else 
            return WDT_ERR;
    }
    else if(type == 0x00) 
        ret = WDT_MINDATA;
    else
        return WDT_ERR;
    //
    if((data[1] & 0x80) == 0x80)
    {
        Mask = 1;
        count = 4;
    }
    else
    {
        Mask = 0;
        count = 0;
    }
    //
    len = data[1] & 0x7F;
    //
    if(len == 126)
    {
        if(dataLen < 4)
            return WDT_ERR;
        len = data[2];
        len = (len << 8) + data[3];
        if(packageLen) *packageLen = len;//转储包长度
        if(packageHeadLen) *packageHeadLen = 4 + count;
        //
        if(dataLen < len + 4 + count)
            return WDT_ERR;
        if(Mask)
        {
            maskKey[0] = data[4];
            maskKey[1] = data[5];
            maskKey[2] = data[6];
            maskKey[3] = data[7];
            dataStart = 8;
        }
        else
            dataStart = 4;
    }
    else if(len == 127)
    {
        if(dataLen < 10)
            return WDT_ERR;
        if(data[2] != 0 || data[3] != 0 || data[4] != 0 || data[5] != 0)    //使用8个字节存储长度时,前4位必须为0,装不下那么多数据...
            return WDT_ERR;
        len = data[6];
        len = (len << 8) + data[7];
        len = (len << 8) + data[8];
        len = (len << 8) + data[9];
        if(packageLen) *packageLen = len;//转储包长度
        if(packageHeadLen) *packageHeadLen = 10 + count;
        //
        if(dataLen < len + 10 + count)
            return WDT_ERR;
        if(Mask)
        {
            maskKey[0] = data[10];
            maskKey[1] = data[11];
            maskKey[2] = data[12];
            maskKey[3] = data[13];
            dataStart = 14;
        }
        else
            dataStart = 10;
    }
    else
    {
        if(packageLen) *packageLen = len;//转储包长度
        if(packageHeadLen) *packageHeadLen = 2 + count;
        //
        if(dataLen < len + 2 + count)
            return WDT_ERR;
        if(Mask)
        {
            maskKey[0] = data[2];
            maskKey[1] = data[3];
            maskKey[2] = data[4];
            maskKey[3] = data[5];
            dataStart = 6;
        }
        else
            dataStart = 2;
    }
    //
    if(dataLen < len + dataStart)
        return WDT_ERR;
    //
    if(packageMaxLen < len + 1)
        return WDT_ERR;
    //
    if(Mask)    // 解包数据使用掩码时, 使用异或解码, maskKey[4]依次和数据异或运算, 逻辑如下
    {
        for(i = 0, count = 0; i < len; i++)
        {
            temp1 = maskKey[count];
            temp2 = data[i + dataStart];
            *package++ =  (char)(((~temp1)&temp2) | (temp1&(~temp2)));  // 异或运算后得到数据
            count += 1;
            if(count >= sizeof(maskKey))    // maskKey[4]循环使用
                count = 0;
        }
        *package = '\0';
    }
    else    // 解包数据没使用掩码, 直接复制数据段
    {
        memcpy(package, &data[dataStart], len);
        package[len] = '\0';
    }
    //
    return ret;
}/*******************************************************************************
 * 名称: webSocket_clientLinkToServer
 * 功能: 向websocket服务器发送http(携带握手key), 以和服务器构建连接, 非阻塞模式
 * 形参: *ip：服务器ip
 *          port : 服务器端口
 *       *interface_path : 接口地址
 * 返回: >0 返回连接句柄      <= 0 连接失败或超时, 所花费的时间 ms
 * 说明: 无
 ******************************************************************************/
int webSocket_clientLinkToServer(char *ip, int port, char *interface_path)
{
    int ret, fd , timeOut;
    int i;
	unsigned char loginBuf[512] = {0}, recBuf[512] = {0}, shakeKey[128] = {0}, *p;
	char tempIp[128] = {0};
	//服务器端网络地址结构体   
	struct sockaddr_in report_addr; 	
    memset(&report_addr,0,sizeof(report_addr)); 			// 数据初始化--清零     
    report_addr.sin_family = AF_INET; 						// 设置为IP通信     
    //report_addr.sin_addr.s_addr = inet_addr(ip);			  
    if((report_addr.sin_addr.s_addr = inet_addr(ip)) == INADDR_NONE)  // 服务器IP地址, 自动域名转换 
    {
        ret = websocket_getIpByHostName(ip, tempIp);
        if(ret < 0)
            return ret;
        else if(strlen(tempIp) < 7)
            return -ret;
        else
            timeOut += ret;
        //
        if((report_addr.sin_addr.s_addr = inet_addr(tempIp)) == INADDR_NONE)
            return -ret;
#ifdef WEBSOCKET_DEBUG
        printf("webSocket_clientLinkToServer : Host(%s) to Ip(%s)\r\n", ip, tempIp);
#endif
    }
    report_addr.sin_port = htons(port); 							// 服务器端口号     
    //
    //printf("webSocket_clientLinkToServer : ip/%s, port/%d path/%s\r\n", ip, port, interface_path);
	//create unix socket  
    if((fd = socket(AF_INET,SOCK_STREAM, 0)) < 0) 
    {  
        printf("webSocket_login : cannot create socket\r\n");  
        return -1;  
    }
    
    // 测试 -----  创建握手key 和 匹配返回key
    // webSocket_buildShakeKey(shakeKey); 
    // printf("key1:%s\r\n", shakeKey);
    // webSocket_buildRespondShakeKey(shakeKey, strlen(shakeKey), shakeKey);
    // printf("key2:%s\r\n", shakeKey);
    
    //非阻塞
    ret = fcntl(fd , F_GETFL , 0);
    fcntl(fd , F_SETFL , ret | O_NONBLOCK);
    
    //connect
    timeOut = 0;
	while(connect(fd , (struct sockaddr *)&report_addr,sizeof(struct sockaddr)) == -1)
	{
		if(++timeOut > WEBSOCKET_LOGIN_CONNECT_TIMEOUT)
		{
			printf("webSocket_login : cannot connect to %s:%d ! %d\r\n" , ip, port, timeOut);
			close(fd); 
		    return -timeOut;  
		}
		webSocket_delayms(1);  //1ms 
	}
	
	//发送http协议头
	memset(shakeKey, 0, sizeof(shakeKey));
	webSocket_buildShakeKey(shakeKey);  // 创建握手key
	
	memset(loginBuf, 0, sizeof(loginBuf));  // 创建协议包
	webSocket_buildHttpHead(ip, port, interface_path, shakeKey, (char *)loginBuf);   
	//发出协议包
    ret = send(fd , loginBuf , strlen((const char*)loginBuf) , MSG_NOSIGNAL);

    //显示http请求
#ifdef WEBSOCKET_DEBUG
	printf("\r\nconnect : %dms\r\nlogin_send:\r\n%s\r\n" , timeOut, loginBuf); 
#endif
	//
    while(1)
    {
		memset(recBuf , 0 , sizeof(recBuf));
		ret = recv(fd , recBuf , sizeof(recBuf) ,  MSG_NOSIGNAL);
		if(ret > 0)
		{
		    if(strncmp((const char *)recBuf, (const char *)"HTTP", strlen((const char *)"HTTP")) == 0)    // 返回的是http回应信息
            {
                //显示http返回
#ifdef WEBSOCKET_DEBUG
                printf("\r\nlogin_recv : %d / %dms\r\n%s\r\n" , ret, timeOut, recBuf); 
#endif
                //
                if((p = (unsigned char *)strstr((const char *)recBuf, (const char *)"Sec-WebSocket-Accept: ")) != NULL) // 定位到握手字符串
                {
                    p += strlen((const char *)"Sec-WebSocket-Accept: ");
                    sscanf((const char *)p, "%s\r\n", p);
                    if(webSocket_matchShakeKey(shakeKey, strlen((const char *)shakeKey), p, strlen((const char *)p)) == 0) // 比对握手信息
                        return fd; // 连接成功, 返回连接句柄fd
                    else
                        ret = send(fd , loginBuf , strlen((const char*)loginBuf) , MSG_NOSIGNAL);    // 握手信号不对, 重发协议包
                }
                else
                    ret = send(fd , loginBuf , strlen((const char*)loginBuf) , MSG_NOSIGNAL);     // 重发协议包
            }
// #ifdef WEBSOCKET_DEBUG
            // 显示异常返回数据
            else
            {
                if(recBuf[0] >= ' ' && recBuf[0] <= '~')
                    printf("\r\nlogin_recv : %d\r\n%s\r\n" , ret, recBuf); 
                else
                {
                    printf("\r\nlogin_recv : %d\r\n" , ret); 
                    for(i = 0; i < ret; i++) 
                        printf("%.2X ", recBuf[i]); 
                    printf("\r\n");
                }
            }
// #endif
		}
		else if(ret <= 0)
		    ;
		if(++timeOut > WEBSOCKET_LOGIN_RESPOND_TIMEOUT)
		{
		    close(fd); 
		    return -timeOut;
		}
		webSocket_delayms(1);  //1ms
	}
    //
	close(fd); 
	return -timeOut;
}
/*******************************************************************************
 * 名称: webSocket_serverLinkToClient
 * 功能: 服务器回复客户端的连接请求, 以建立websocket连接
 * 形参: fd：连接句柄
 *      *recvBuf : 接收到来自客户端的数据(内含http连接请求)
 *      bufLen : 
 * 返回: >0 建立websocket连接成功 <=0 建立websocket连接失败
 * 说明: 无
 ******************************************************************************/
int webSocket_serverLinkToClient(int fd, char *recvBuf, int bufLen)
{
    char *p;
    int ret;
    char recvShakeKey[512], respondPackage[1024];
    if((p = strstr(recvBuf, "Sec-WebSocket-Key: ")) == NULL)
        return -1;
    p += strlen("Sec-WebSocket-Key: ");
    //
    memset(recvShakeKey, 0, sizeof(recvShakeKey));
    sscanf(p, "%s", recvShakeKey);      // 取得握手key
    ret = strlen(recvShakeKey);
    if(ret < 1)
        return -1;
    //
    memset(respondPackage, 0, sizeof(respondPackage));
    webSocket_buildHttpRespond((unsigned char *)recvShakeKey, (unsigned int)ret, respondPackage);
    //
    return send(fd, respondPackage, strlen(respondPackage), MSG_NOSIGNAL);
}
/*******************************************************************************
 * 名称: webSocket_send
 * 功能: websocket数据基本打包和发送
 * 形参: fd：连接句柄
 *          *data : 数据
 *          dataLen : 长度
 *          isMask : 数据是否使用掩码, 客户端到服务器必须使用掩码模式
 *          type : 数据要要以什么识别头类型发送(txt, bin, ping, pong ...)
 * 返回: 调用send的返回
 * 说明: 无
 ******************************************************************************/
int webSocket_send(int fd, char *data, int dataLen, bool isMask, WebsocketData_Type type)
{
    unsigned char *webSocketPackage = NULL;
    int retLen, ret;
#ifdef WEBSOCKET_DEBUG
    unsigned int i;
    printf("webSocket_send : %d\r\n", dataLen);
#endif
    //---------- websocket数据打包 ----------
    webSocketPackage = (unsigned char *)calloc(dataLen + 128, sizeof(char));
    retLen = webSocket_enPackage((unsigned char *)data, dataLen, webSocketPackage, (dataLen + 128), isMask, type);
    //显示数据
#ifdef WEBSOCKET_DEBUG
    printf("webSocket_send : %d\r\n" , retLen);
    for(i = 0; i < retLen; i ++)  
        printf("%.2X ", webSocketPackage[i]);
    printf("\r\n");
#endif
    //
    ret = send(fd, webSocketPackage, retLen, MSG_NOSIGNAL);
    free(webSocketPackage);
    return ret;
}
/*******************************************************************************
 * 名称: webSocket_recv
 * 功能: websocket数据接收和基本解包
 * 形参: fd：连接句柄
 *          *data : 数据接收地址
 *          dataMaxLen : 接收区可用最大长度
 * 返回: = 0 没有收到有效数据 > 0 成功接收并解包数据 < 0 非包数据的长度
 * 说明: 无
 ******************************************************************************/
int webSocket_recv(int fd, char *data, int dataMaxLen, int *dataType)
{
    unsigned char *webSocketPackage = NULL, *recvBuf = NULL;
    int ret, dpRet = WDT_NULL, retTemp, retFinal = 0;
    int retLen = 0, retHeadLen = 0;
    //
    int timeOut = 0;    //续传时,等待下一包需加时间限制
    
    recvBuf = (unsigned char *)calloc(dataMaxLen, sizeof(char));
    ret = recv(fd, recvBuf, dataMaxLen, MSG_NOSIGNAL);
    //数据可能超出了范围限制
    if(ret == dataMaxLen)
        printf("webSocket_recv : warning !! recv buff too large !! (recv/%d)\r\n", ret);
    //
    if(ret > 0)
    {
        //---------- websocket数据解包 ----------
        webSocketPackage = (unsigned char *)calloc(ret + 128, sizeof(char));
        dpRet = webSocket_dePackage(recvBuf, ret, webSocketPackage, (ret + 128), (unsigned int *)&retLen, (unsigned int *)&retHeadLen);
        if(dpRet == WDT_ERR && retLen == 0) //非包数据
        {
            memset(data, 0, dataMaxLen);
            if(ret < dataMaxLen)
            {
                memcpy(data, recvBuf, ret);
                retFinal = -ret;
            }
            else
            {
                memcpy(data, recvBuf, dataMaxLen);
                retFinal = -dataMaxLen;
            }
        }
        else //正常收包
        {
            //数据可能超出了范围限制
            if(retLen > dataMaxLen)
            {
                printf("webSocket_recv : warning !! recv package too large !! (recvPackage/%d)\r\n", retLen);
                goto recv_return_null;
            }
            //显示数据包的头10个字节
#ifdef WEBSOCKET_DEBUG
            if(ret > 10)
                printf("webSocket_recv : ret/%d, dpRet/%d, retLen/%d, head/%d : %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X\r\n", 
                    ret, dpRet, retLen, retHeadLen, 
                    recvBuf[0], recvBuf[1], recvBuf[2], recvBuf[3], recvBuf[4], 
                    recvBuf[5], recvBuf[6], recvBuf[7], recvBuf[8], recvBuf[9]);
#endif
            
            //续传? 检查数据包的头10个字节发现recv()时并没有把一包数据接收完,继续接收..
            if(ret < retHeadLen + retLen)
            {
                timeOut = 50;//50*10=500ms等待
                while(ret < retHeadLen + retLen)
                {
                    webSocket_delayms(10);
                    retTemp = recv(fd, &recvBuf[ret], dataMaxLen - ret, MSG_NOSIGNAL);
                    if(retTemp > 0){
                        timeOut = 50;//50*10=500ms等待
                        ret += retTemp;
                    }else{
                        if(errno == EAGAIN || errno == EINTR);//连接中断
                        else goto recv_return_null;
                    }
                    if(--timeOut < 1) 
                        goto recv_return_null;
                }
                //再解包一次
                free(webSocketPackage);
                webSocketPackage = (unsigned char *)calloc(ret + 128, sizeof(char));
                dpRet = webSocket_dePackage(recvBuf, ret, webSocketPackage, (ret + 128), (unsigned int *)&retLen, (unsigned int *)&retHeadLen);
                //
                if(ret > 10)
                    printf("webSocket_recv : ret/%d, dpRet/%d, retLen/%d, head/%d : %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X\r\n", 
                        ret, dpRet, retLen, retHeadLen, 
                        recvBuf[0], recvBuf[1], recvBuf[2], recvBuf[3], recvBuf[4], 
                        recvBuf[5], recvBuf[6], recvBuf[7], recvBuf[8], recvBuf[9]);
            }
            //
            if(retLen > 0)
            {
                if(dpRet == WDT_PING)
                {
                    webSocket_send(fd, (char *)webSocketPackage, retLen, true, WDT_PONG);//自动 ping-pong
                    // 显示数据
                    printf("webSocket_recv : PING %d\r\n%s\r\n" , retLen, webSocketPackage);
                }
                else if(dpRet == WDT_PONG)
                {
                    printf("webSocket_recv : PONG %d\r\n%s\r\n" , retLen, webSocketPackage);
                }
                else //if(dpRet == WDT_TXTDATA || dpRet == WDT_BINDATA || dpRet == WDT_MINDATA)
                {
                    memcpy(data, webSocketPackage, retLen);
                    // 显示数据
#ifdef WEBSOCKET_DEBUG
                    if(webSocketPackage[0] >= ' ' && webSocketPackage[0] <= '~')
                        printf("\r\nwebSocket_recv : New Package StrFile dpRet:%d/retLen:%d\r\n%s\r\n" , dpRet, retLen, webSocketPackage); 
                    else
                    {
                        printf("\r\nwebSocket_recv : New Package BinFile dpRet:%d/retLen:%d\r\n" , dpRet, retLen); 
                        int i;
                        for(i = 0; i < retLen; i++) 
                            printf("%.2X ", webSocketPackage[i]); 
                        printf("\r\n");
                    }
#endif
                }
                //
                retFinal = retLen;
            }
#ifdef WEBSOCKET_DEBUG
            else
            {
                // 显示数据
                if(recvBuf[0] >= ' ' && recvBuf[0] <= '~') 
                    printf("\r\nwebSocket_recv : ret:%d/dpRet:%d/retLen:%d\r\n%s\r\n" , ret, dpRet, retLen, recvBuf); 
                else 
                {
                    printf("\r\nwebSocket_recv : ret:%d/dpRet:%d/retLen:%d\r\n%s\r\n" , ret, dpRet, retLen, recvBuf); 
                    int i;
                    for(i = 0; i < ret; i++) 
                        printf("%.2X ", recvBuf[i]); 
                    printf("\r\n");
                }
            }
#endif
        }
    }

    if(recvBuf) free(recvBuf);
    if(webSocketPackage) free(webSocketPackage);
    if(dataType) *dataType = dpRet;
    return retFinal;

recv_return_null:

    if(recvBuf) free(recvBuf);
    if(webSocketPackage) free(webSocketPackage);
    if(dataType) *dataType = dpRet;
    return 0;
}
