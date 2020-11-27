
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h> // 使用 malloc, calloc等动态分配内存方法
#include <time.h>   // 获取系统时间
#include <errno.h>
#include <pthread.h>
#include <fcntl.h> // 非阻塞
#include <sys/un.h>
#include <arpa/inet.h>  // inet_addr()
#include <unistd.h>     // close()
#include <sys/types.h>  // 文件IO操作
#include <sys/socket.h> //
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h> // gethostbyname, gethostbyname2, gethostbyname_r, gethostbyname_r2
#include <sys/un.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h> // SIOCSIFADDR

#include "ws_com.h"

// 生成握手key的长度
#define WEBSOCKET_SHAKE_KEY_LEN 16

void ws_delayms(int ms)
{
    struct timeval tim;
    tim.tv_sec = ms / 1000;
    tim.tv_usec = ms % 1000 * 1000;
    select(0, NULL, NULL, NULL, &tim);
}

//==================== 域名转IP ====================

typedef struct
{
    pthread_t thread_id;
    char ip[256];
    bool result;
    bool actionEnd;
} GetHostName_Struct;

void *ws_getHost_fun(void *arge)
{
    int32_t ret;
    //int32_t i;
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
    if (gethostbyname_r(gs->ip, &host_body, buf, sizeof(buf), &host, &ret))
    {
        gs->actionEnd = true;
        return NULL;
    }
    if (host == NULL)
    {
        gs->actionEnd = true;
        return NULL;
    }
    addr_list = (struct in_addr **)host->h_addr_list;
    //printf("ip name: %s\r\nip list: ", host->h_name);
    //for(i = 0; addr_list[i] != NULL; i++) printf("%s, ", inet_ntoa(*addr_list[i])); printf("\r\n");
    if (addr_list[0] == NULL)
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

//域名转IP工具,成功返回大于0请求时长ms,失败返回负值的请求时长ms
int ws_getIpByHostName(char *hostName, char *backIp)
{
    int32_t i, timeoutCount = 1;
    GetHostName_Struct gs;
    if (hostName == NULL)
        return -1;
    else if (strlen(hostName) < 1)
        return -1;
    //开线程从域名获取IP
    memset(&gs, 0, sizeof(GetHostName_Struct));
    strcpy(gs.ip, hostName);
    gs.result = false;
    gs.actionEnd = false;
    if (pthread_create(&gs.thread_id, NULL, (void *)ws_getHost_fun, &gs) < 0)
        return -1;
    i = 0;
    while (!gs.actionEnd)
    {
        if (++i > 10)
        {
            i = 0;
            if (++timeoutCount > 1000)
                break;
        }
        ws_delayms(1000); // 1ms延时
    }
    // pthread_cancel(gs.thread_id);
    pthread_join(gs.thread_id, NULL);
    if (!gs.result)
        return -timeoutCount;
    //开线程从域名获取IP
    memset(backIp, 0, strlen((const char *)backIp));
    strcpy(backIp, gs.ip);
    return timeoutCount;
}

//==================== 加密方法BASE64 ====================

//base64编/解码用的基础字符集
const char ws_base64char[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*******************************************************************************
 * 名称: ws_base64_encode
 * 功能: ascii编码为base64格式
 * 参数: 
 *      bindata: ascii字符串输入
 *      base64: base64字符串输出
 *      binlength: bindata的长度
 * 返回: base64字符串长度
 * 说明: 无
 ******************************************************************************/
int32_t ws_base64_encode(const unsigned char *bindata, char *base64, int32_t binlength)
{
    int32_t i, j;
    unsigned char current;
    for (i = 0, j = 0; i < binlength; i += 3)
    {
        current = (bindata[i] >> 2);
        current &= (unsigned char)0x3F;
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((unsigned char)(bindata[i] << 4)) & ((unsigned char)0x30);
        if (i + 1 >= binlength)
        {
            base64[j++] = ws_base64char[(int32_t)current];
            base64[j++] = '=';
            base64[j++] = '=';
            break;
        }
        current |= ((unsigned char)(bindata[i + 1] >> 4)) & ((unsigned char)0x0F);
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((unsigned char)(bindata[i + 1] << 2)) & ((unsigned char)0x3C);
        if (i + 2 >= binlength)
        {
            base64[j++] = ws_base64char[(int32_t)current];
            base64[j++] = '=';
            break;
        }
        current |= ((unsigned char)(bindata[i + 2] >> 6)) & ((unsigned char)0x03);
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((unsigned char)bindata[i + 2]) & ((unsigned char)0x3F);
        base64[j++] = ws_base64char[(int32_t)current];
    }
    base64[j] = '\0';
    return j;
}
/*******************************************************************************
 * 名称: ws_base64_decode
 * 功能: base64格式解码为ascii
 * 参数: 
 *      base64: base64字符串输入
 *      bindata: ascii字符串输出
 * 返回: 解码出来的ascii字符串长度
 * 说明: 无
 ******************************************************************************/
int32_t ws_base64_decode(const char *base64, unsigned char *bindata)
{
    int32_t i, j;
    unsigned char k;
    unsigned char temp[4];
    for (i = 0, j = 0; base64[i] != '\0'; i += 4)
    {
        memset(temp, 0xFF, sizeof(temp));
        for (k = 0; k < 64; k++)
        {
            if (ws_base64char[k] == base64[i])
                temp[0] = k;
        }
        for (k = 0; k < 64; k++)
        {
            if (ws_base64char[k] == base64[i + 1])
                temp[1] = k;
        }
        for (k = 0; k < 64; k++)
        {
            if (ws_base64char[k] == base64[i + 2])
                temp[2] = k;
        }
        for (k = 0; k < 64; k++)
        {
            if (ws_base64char[k] == base64[i + 3])
                temp[3] = k;
        }
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[0] << 2)) & 0xFC)) |
                       ((unsigned char)((unsigned char)(temp[1] >> 4) & 0x03));
        if (base64[i + 2] == '=')
            break;
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[1] << 4)) & 0xF0)) |
                       ((unsigned char)((unsigned char)(temp[2] >> 2) & 0x0F));
        if (base64[i + 3] == '=')
            break;
        bindata[j++] = ((unsigned char)(((unsigned char)(temp[2] << 6)) & 0xF0)) |
                       ((unsigned char)(temp[3] & 0x3F));
    }
    return j;
}

//==================== 加密方法 sha1哈希 ====================

typedef struct SHA1Context
{
    uint32_t Message_Digest[5];
    uint32_t Length_Low;
    uint32_t Length_High;
    unsigned char Message_Block[64];
    int32_t Message_Block_Index;
    int32_t Computed;
    int32_t Corrupted;
} SHA1Context;

#define SHA1CircularShift(bits, word) ((((word) << (bits)) & 0xFFFFFFFF) | ((word) >> (32 - (bits))))

void SHA1ProcessMessageBlock(SHA1Context *context)
{
    const uint32_t K[] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};
    int32_t t;
    uint32_t temp;
    uint32_t W[80];
    uint32_t A, B, C, D, E;

    for (t = 0; t < 16; t++)
    {
        W[t] = ((uint32_t)context->Message_Block[t * 4]) << 24;
        W[t] |= ((uint32_t)context->Message_Block[t * 4 + 1]) << 16;
        W[t] |= ((uint32_t)context->Message_Block[t * 4 + 2]) << 8;
        W[t] |= ((uint32_t)context->Message_Block[t * 4 + 3]);
    }

    for (t = 16; t < 80; t++)
        W[t] = SHA1CircularShift(1, W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]);

    A = context->Message_Digest[0];
    B = context->Message_Digest[1];
    C = context->Message_Digest[2];
    D = context->Message_Digest[3];
    E = context->Message_Digest[4];

    for (t = 0; t < 20; t++)
    {
        temp = SHA1CircularShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        temp &= 0xFFFFFFFF;
        E = D;
        D = C;
        C = SHA1CircularShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 20; t < 40; t++)
    {
        temp = SHA1CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
        temp &= 0xFFFFFFFF;
        E = D;
        D = C;
        C = SHA1CircularShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 40; t < 60; t++)
    {
        temp = SHA1CircularShift(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        temp &= 0xFFFFFFFF;
        E = D;
        D = C;
        C = SHA1CircularShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 60; t < 80; t++)
    {
        temp = SHA1CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
        temp &= 0xFFFFFFFF;
        E = D;
        D = C;
        C = SHA1CircularShift(30, B);
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
    context->Length_Low = 0;
    context->Length_High = 0;
    context->Message_Block_Index = 0;

    context->Message_Digest[0] = 0x67452301;
    context->Message_Digest[1] = 0xEFCDAB89;
    context->Message_Digest[2] = 0x98BADCFE;
    context->Message_Digest[3] = 0x10325476;
    context->Message_Digest[4] = 0xC3D2E1F0;

    context->Computed = 0;
    context->Corrupted = 0;
}

void SHA1PadMessage(SHA1Context *context)
{
    if (context->Message_Block_Index > 55)
    {
        context->Message_Block[context->Message_Block_Index++] = 0x80;
        while (context->Message_Block_Index < 64)
            context->Message_Block[context->Message_Block_Index++] = 0;
        SHA1ProcessMessageBlock(context);
        while (context->Message_Block_Index < 56)
            context->Message_Block[context->Message_Block_Index++] = 0;
    }
    else
    {
        context->Message_Block[context->Message_Block_Index++] = 0x80;
        while (context->Message_Block_Index < 56)
            context->Message_Block[context->Message_Block_Index++] = 0;
    }
    context->Message_Block[56] = (context->Length_High >> 24) & 0xFF;
    context->Message_Block[57] = (context->Length_High >> 16) & 0xFF;
    context->Message_Block[58] = (context->Length_High >> 8) & 0xFF;
    context->Message_Block[59] = (context->Length_High) & 0xFF;
    context->Message_Block[60] = (context->Length_Low >> 24) & 0xFF;
    context->Message_Block[61] = (context->Length_Low >> 16) & 0xFF;
    context->Message_Block[62] = (context->Length_Low >> 8) & 0xFF;
    context->Message_Block[63] = (context->Length_Low) & 0xFF;

    SHA1ProcessMessageBlock(context);
}

int32_t SHA1Result(SHA1Context *context)
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

void SHA1Input(SHA1Context *context, const char *message_array, uint32_t length)
{
    if (!length)
        return;

    if (context->Computed || context->Corrupted)
    {
        context->Corrupted = 1;
        return;
    }

    while (length-- && !context->Corrupted)
    {
        context->Message_Block[context->Message_Block_Index++] = (*message_array & 0xFF);

        context->Length_Low += 8;

        context->Length_Low &= 0xFFFFFFFF;
        if (context->Length_Low == 0)
        {
            context->Length_High++;
            context->Length_High &= 0xFFFFFFFF;
            if (context->Length_High == 0)
                context->Corrupted = 1;
        }

        if (context->Message_Block_Index == 64)
        {
            SHA1ProcessMessageBlock(context);
        }
        message_array++;
    }
}

/* int32_t sha1_hash(const char *source, char *lrvar){// Main 
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
} */

char *sha1_hash(const char *source)
{
    SHA1Context sha;
    char *buf; //[128];

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
        sprintf(buf, "%08X%08X%08X%08X%08X", sha.Message_Digest[0], sha.Message_Digest[1],
                sha.Message_Digest[2], sha.Message_Digest[3], sha.Message_Digest[4]);
        //lr_save_string(buf, lrvar);
        //return strlen(buf);
        return buf;
    }
}

int32_t tolower(int32_t c)
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

int32_t htoi(const char s[], int32_t start, int32_t len)
{
    int32_t i, j;
    int32_t n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) //判断是否有前导0x或者0X
    {
        i = 2;
    }
    else
    {
        i = 0;
    }
    i += start;
    j = 0;
    for (; (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'); ++i)
    {
        if (j >= len)
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

//==================== websocket部分 ====================

/*******************************************************************************
 * 名称: ws_getRandomString
 * 功能: 生成随机字符串
 * 参数: 
 *      buff：随机字符串存储到
 *      len: 生成随机字符串长度
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void ws_getRandomString(char *buff, uint32_t len)
{
    uint32_t i;
    unsigned char temp;
    srand((int32_t)time(0));
    for (i = 0; i < len; i++)
    {
        temp = (unsigned char)(rand() % 256);
        if (temp == 0) // 随机数不要0, 0 会干扰对字符串长度的判断
            temp = 128;
        buff[i] = temp;
    }
}

/*******************************************************************************
 * 名称: ws_buildShakeKey
 * 功能: client端使用随机数构建握手用的key
 * 参数: *key：随机生成的握手key
 * 返回: key的长度
 * 说明: 无
 ******************************************************************************/
int32_t ws_buildShakeKey(char *key)
{
    char tempKey[WEBSOCKET_SHAKE_KEY_LEN] = {0};
    ws_getRandomString(tempKey, WEBSOCKET_SHAKE_KEY_LEN);
    return ws_base64_encode((const unsigned char *)tempKey, (char *)key, WEBSOCKET_SHAKE_KEY_LEN);
}

/*******************************************************************************
 * 名称: ws_buildRespondShakeKey
 * 功能: server端在接收client端的key后,构建回应用的key
 * 参数:
 *      acceptKey：来自客户端的key字符串
 *      acceptKeyLen: 长度
 *      respondKey:  在 acceptKey 之后加上 GUID, 再sha1哈希, 再转成base64得到 respondKey
 * 返回: respondKey的长度(肯定比acceptKey要长)
 * 说明: 无
 ******************************************************************************/
int32_t ws_buildRespondShakeKey(char *acceptKey, uint32_t acceptKeyLen, char *respondKey)
{
    char *clientKey;
    char *sha1DataTemp;
    char *sha1Data;
    int32_t i, n;
    const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint32_t GUIDLEN;

    if (acceptKey == NULL)
        return 0;
    GUIDLEN = sizeof(GUID);
    clientKey = (char *)calloc(acceptKeyLen + GUIDLEN + 10, sizeof(char));
    memset(clientKey, 0, (acceptKeyLen + GUIDLEN + 10));

    memcpy(clientKey, acceptKey, acceptKeyLen);
    memcpy(&clientKey[acceptKeyLen], GUID, GUIDLEN);
    clientKey[acceptKeyLen + GUIDLEN] = '\0';

    sha1DataTemp = sha1_hash(clientKey);
    n = strlen((const char *)sha1DataTemp);
    sha1Data = (char *)calloc(n / 2 + 1, sizeof(char));
    memset(sha1Data, 0, n / 2 + 1);

    for (i = 0; i < n; i += 2)
        sha1Data[i / 2] = htoi(sha1DataTemp, i, 2);
    n = ws_base64_encode((const unsigned char *)sha1Data, (char *)respondKey, (n / 2));

    free(sha1DataTemp);
    free(sha1Data);
    free(clientKey);
    return n;
}

/*******************************************************************************
 * 名称: ws_matchShakeKey
 * 功能: client端收到来自服务器回应的key后进行匹配,以验证握手成功
 * 参数:
 *      myKey：client端请求握手时发给服务器的key
 *      myKeyLen: 长度
 *      acceptKey: 服务器回应的key
 *      acceptKeyLen: 长度
 * 返回: 0 成功  -1 失败
 * 说明: 无
 ******************************************************************************/
int32_t ws_matchShakeKey(char *myKey, uint32_t myKeyLen, char *acceptKey, uint32_t acceptKeyLen)
{
    int32_t retLen;
    char tempKey[256] = {0};

    retLen = ws_buildRespondShakeKey(myKey, myKeyLen, tempKey);
    //printf("ws_matchShakeKey :\r\n%d: %s\r\n%d: %s\r\n", acceptKeyLen, acceptKey, retLen, tempKey);

    if (retLen != acceptKeyLen)
    {
        printf("ws_matchShakeKey: len err\r\n%s\r\n%s\r\n%s\r\n", myKey, tempKey, acceptKey);
        return -1;
    }
    else if (strcmp((const char *)tempKey, (const char *)acceptKey) != 0)
    {
        printf("ws_matchShakeKey: str err\r\n%s\r\n%s\r\n", tempKey, acceptKey);
        return -1;
    }
    return 0;
}

/*******************************************************************************
 * 名称: ws_buildHttpHead
 * 功能: 构建client端连接服务器时的http协议头, 注意websocket是GET形式的
 * 参数:
 *      ip：要连接的服务器ip字符串
 *      port: 服务器端口
 *      path: 要连接的端口地址
 *      shakeKey: 握手key, 可以由任意的16位字符串打包成base64后得到
 *      package: 存储最后打包好的内容
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void ws_buildHttpHead(char *ip, int32_t port, char *path, char *shakeKey, char *package)
{
    const char httpDemo[] =
        "GET %s HTTP/1.1\r\n"
        "Connection: Upgrade\r\n"
        "Host: %s:%d\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Upgrade: websocket\r\n\r\n";
    sprintf(package, httpDemo, path, ip, port, shakeKey);
}

/*******************************************************************************
 * 名称: ws_buildHttpRespond
 * 功能: 构建server端回复client连接请求的http协议
 * 参数:
 *      acceptKey：来自client的握手key
 *      acceptKeyLen: 长度
 *      package: 存储
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
void ws_buildHttpRespond(char *acceptKey, uint32_t acceptKeyLen, char *package)
{
    const char httpDemo[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Server: Microsoft-HTTPAPI/2.0\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s\r\n\r\n"; // 时间打包待续, 格式如 "Date: Tue, 20 Jun 2017 08:50:41 CST\r\n"
    time_t now;
    struct tm *tm_now;
    char timeStr[256] = {0};
    char respondShakeKey[256] = {0};
    // 构建回应的握手key
    ws_buildRespondShakeKey(acceptKey, acceptKeyLen, respondShakeKey);
    // 构建回应时间字符串
    time(&now);
    tm_now = localtime(&now);
    strftime(timeStr, sizeof(timeStr), "Date: %a, %d %b %Y %T %Z", tm_now);
    // 组成回复信息
    sprintf(package, httpDemo, respondShakeKey, timeStr);
}

/*******************************************************************************
 * 名称: ws_enPackage
 * 功能: websocket数据收发阶段的数据打包, 通常client发server的数据都要mask(掩码)处理, 反之server到client却不用
 * 参数:
 *      data：准备发出的数据
 *      dataLen: 长度
 *      package: 打包后存储地址
 *      packageMaxLen: 存储地址可用长度
 *      mask: 是否使用掩码     1要   0 不要
 *      type: 数据类型, 由打包后第一个字节决定, 这里默认是数据传输, 即0x81
 * 返回: 打包后的长度(会比原数据长2~16个字节不等)      <=0 打包失败 
 * 说明: 无
 ******************************************************************************/
int32_t ws_enPackage(
    unsigned char *data, uint32_t dataLen,
    unsigned char *package, uint32_t packageMaxLen,
    bool mask, WsData_Type type)
{
    unsigned char maskKey[4] = {0}; // 掩码
    int32_t count;
    uint32_t i, len = 0;
    //最小长度检查
    if (packageMaxLen < 2)
        return -1;
    //根据包类型设置头字节
    if (type == WDT_MINDATA)
        *package++ = 0x00;
    else if (type == WDT_TXTDATA)
        *package++ = 0x81;
    else if (type == WDT_BINDATA)
        *package++ = 0x82;
    else if (type == WDT_DISCONN)
        *package++ = 0x88;
    else if (type == WDT_PING)
        *package++ = 0x89;
    else if (type == WDT_PONG)
        *package++ = 0x8A;
    else
        return -1;
    //掩码位
    if (mask)
        *package = 0x80;
    len += 1;
    //半字节记录长度
    if (dataLen < 126)
    {
        *package++ |= (dataLen & 0x7F);
        len += 1;
    }
    //2字节记录长度
    else if (dataLen < 65536)
    {
        if (packageMaxLen < 4)
            return -1;
        *package++ |= 0x7E;
        *package++ = (unsigned char)((dataLen >> 8) & 0xFF);
        *package++ = (unsigned char)((dataLen >> 0) & 0xFF);
        len += 3;
    }
    //8字节记录长度
    else if (dataLen < 0xFFFFFFFF)
    {
        if (packageMaxLen < 10)
            return -1;
        *package++ |= 0x7F;
        *package++ = 0;                                 //(unsigned char)((dataLen >> 56) & 0xFF); // 数据长度变量是 uint32_t dataLen, 暂时没有那么多数据
        *package++ = 0;                                 //(unsigned char)((dataLen >> 48) & 0xFF);
        *package++ = 0;                                 //(unsigned char)((dataLen >> 40) & 0xFF);
        *package++ = 0;                                 //(unsigned char)((dataLen >> 32) & 0xFF);
        *package++ = (unsigned char)((dataLen >> 24) & 0xFF); // 到这里就够传4GB数据了
        *package++ = (unsigned char)((dataLen >> 16) & 0xFF);
        *package++ = (unsigned char)((dataLen >> 8) & 0xFF);
        *package++ = (unsigned char)((dataLen >> 0) & 0xFF);
        len += 9;
    }
    // 数据使用掩码时,使用异或解码,maskKey[4]依次和数据异或运算,逻辑如下
    if (mask)
    {
        if (packageMaxLen < len + dataLen + 4)
            return -1;
        //随机生成掩码
        ws_getRandomString((char *)maskKey, sizeof(maskKey));
        *package++ = maskKey[0];
        *package++ = maskKey[1];
        *package++ = maskKey[2];
        *package++ = maskKey[3];
        len += 4;
        for (i = 0, count = 0; i < dataLen; i++)
        {
            //异或运算后得到数据
            *package++ = maskKey[count] ^ data[i];
            count += 1;
            //maskKey[4]循环使用
            if (count >= sizeof(maskKey))
                count = 0;
        }
        len += i;
        *package = '\0';
    }
    // 数据没使用掩码, 直接复制数据段
    else
    {
        if (packageMaxLen < len + dataLen)
            return -1;
        memcpy(package, data, dataLen);
        package[dataLen] = '\0';
        len += dataLen;
    }

    return len;
}

/*******************************************************************************
 * 名称: ws_dePackage
 * 功能: websocket数据收发阶段的数据解包, 通常client发server的数据都要mask(掩码)处理, 反之server到client却不用
 * 参数:
 *      data：解包的数据
 *      dataLen: 长度
 *      package: 解包后存储地址
 *      packageMaxLen: 存储地址可用长度
 *      packageLen: 解包所得长度
 * 返回: 解包识别的数据类型 如: txt数据, bin数据, ping, pong等
 * 说明: 无
 ******************************************************************************/
WsData_Type ws_dePackage(
    unsigned char *data, uint32_t dataLen,
    unsigned char *package, uint32_t packageMaxLen,
    uint32_t *packageLen, uint32_t *packageHeadLen)
{
    unsigned char maskKey[4] = {0}; // 掩码
    char mask = 0, type;
    int32_t count, ret;
    uint32_t i, len = 0, dataOffset = 2;

    if (dataLen < 2)
        return WDT_ERR;
    //解析包类型
    type = data[0] & 0x0F;
    if ((data[0] & 0x80) == 0x80)
    {
        if (type == 0x01)
            ret = WDT_TXTDATA;
        else if (type == 0x02)
            ret = WDT_BINDATA;
        else if (type == 0x08)
            ret = WDT_DISCONN;
        else if (type == 0x09)
            ret = WDT_PING;
        else if (type == 0x0A)
            ret = WDT_PONG;
        else
            return WDT_ERR;
    }
    else if (type == 0x00)
        ret = WDT_MINDATA;
    else
        return WDT_ERR;
    //是否掩码,及长度占用字节数
    if ((data[1] & 0x80) == 0x80)
    {
        mask = 1;
        count = 4;
    }
    else
    {
        mask = 0;
        count = 0;
    }
    len = data[1] & 0x7F;
    //2字节记录长度
    if (len == 126)
    {
        //数据长度不足
        if (dataLen < 4)
            return WDT_ERR;
        //2字节记录长度
        len = data[2];
        len = (len << 8) + data[3];
        if (packageLen)
            *packageLen = len; //转储包长度
        if (packageHeadLen)
            *packageHeadLen = 4 + count;
        //数据长度不足
        if (dataLen < len + 4 + count)
            return WDT_ERR;
        //获得掩码
        if (mask)
        {
            maskKey[0] = data[4];
            maskKey[1] = data[5];
            maskKey[2] = data[6];
            maskKey[3] = data[7];
            dataOffset = 8;
        }
        else
            dataOffset = 4;
    }
    //8字节记录长度
    else if (len == 127)
    {
        //数据长度不足
        if (dataLen < 10)
            return WDT_ERR;
        //使用8个字节存储长度时,前4位必须为0,装不下那么多数据...
        if (data[2] != 0 || data[3] != 0 || data[4] != 0 || data[5] != 0)
            return WDT_ERR;
        //8字节记录长度
        len = data[6];
        len = (len << 8) + data[7];
        len = (len << 8) + data[8];
        len = (len << 8) + data[9];
        //转储包长度
        if (packageLen)
            *packageLen = len;
        if (packageHeadLen)
            *packageHeadLen = 10 + count;
        //数据长度不足
        if (dataLen < len + 10 + count)
            return WDT_ERR;
        //获得掩码
        if (mask)
        {
            maskKey[0] = data[10];
            maskKey[1] = data[11];
            maskKey[2] = data[12];
            maskKey[3] = data[13];
            dataOffset = 14;
        }
        else
            dataOffset = 10;
    }
    //半字节记录长度
    else
    {
        //转储包长度
        if (packageLen)
            *packageLen = len;
        if (packageHeadLen)
            *packageHeadLen = 2 + count;
        //数据长度不足
        if (dataLen < len + 2 + count)
            return WDT_ERR;
        //获得掩码
        if (mask)
        {
            maskKey[0] = data[2];
            maskKey[1] = data[3];
            maskKey[2] = data[4];
            maskKey[3] = data[5];
            dataOffset = 6;
        }
        else
            dataOffset = 2;
    }
    //数据长度不足
    if (dataLen < len + dataOffset)
        return WDT_ERR;
    //收包地址不足以存下这包数据
    if (packageMaxLen < len + 1)
        return WDT_ERR;
    // 解包数据使用掩码时, 使用异或解码, maskKey[4]依次和数据异或运算, 逻辑如下
    if (mask)
    {
        for (i = 0, count = 0; i < len; i++)
        {
            // 异或运算后得到数据
            *package++ = maskKey[count] ^ data[i + dataOffset];
            count += 1;
            // maskKey[4]循环使用
            if (count >= sizeof(maskKey))
                count = 0;
        }
        *package = '\0';
    }
    // 解包数据没使用掩码, 直接复制数据段
    else
    {
        memcpy(package, &data[dataOffset], len);
        package[len] = '\0';
    }

    return ret;
}

/*******************************************************************************
 * 名称: ws_connectToServer
 * 功能: 向websocket服务器发送http(携带握手key), 以和服务器构建连接, 非阻塞模式
 * 参数:
 *      ip： 服务器ip
 *      port: 服务器端口
 *      path: 接口地址
 *      timeoutMs: connect阶段超时设置,接收阶段为timeoutMs*2,写0使用默认值1000
 * 返回: >0 返回连接控制符 <= 0 连接失败或超时, 所花费的时间 ms
 * 说明: 无
 ******************************************************************************/
int ws_connectToServer(char *ip, int port, char *path, int timeoutMs)
{
    int32_t ret, fd;
    int32_t timeoutCount = 0;
    int32_t i;
    char retBuff[512] = {0};
    char httpHead[512] = {0};
    char shakeKey[128] = {0};
    char *p;
    char tempIp[128] = {0};

    //服务器端网络地址结构体
    struct sockaddr_in report_addr;
    memset(&report_addr, 0, sizeof(report_addr)); // 数据初始化--清零
    report_addr.sin_family = AF_INET;             // 设置为IP通信
    report_addr.sin_port = htons(port);           // 服务器端口号

    // 服务器IP地址, 自动域名转换
    //report_addr.sin_addr.s_addr = inet_addr(ip);
    if ((report_addr.sin_addr.s_addr = inet_addr(ip)) == INADDR_NONE)
    {
        ret = ws_getIpByHostName(ip, tempIp);
        if (ret < 0)
            return ret;
        else if (strlen((const char *)tempIp) < 7)
            return -ret;
        else
            timeoutCount += ret;
        if ((report_addr.sin_addr.s_addr = inet_addr(tempIp)) == INADDR_NONE)
            return -ret;
#ifdef WS_DEBUG
        printf("ws_connectToServer: Host(%s) to Ip(%s)\r\n", ip, tempIp);
#endif
    }

    //默认超时1秒
    if (timeoutMs == 0)
        timeoutMs = 1000;

    //create unix socket
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("ws_connectToServer: cannot create socket\r\n");
        return -1;
    }

    // 测试 -----  创建握手key 和 匹配返回key
    // ws_buildShakeKey(shakeKey);
    // printf("key1:%s\r\n", shakeKey);
    // ws_buildRespondShakeKey(shakeKey, strlen(shakeKey), shakeKey);
    // printf("key2:%s\r\n", shakeKey);

    //非阻塞
    ret = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, ret | O_NONBLOCK);

    //connect
    timeoutCount = 0;
    while (connect(fd, (struct sockaddr *)&report_addr, sizeof(struct sockaddr)) != 0)
    {
        if (++timeoutCount > timeoutMs)
        {
            printf("ws_connectToServer: cannot connect to %s:%d timeout %d\r\n", ip, port, timeoutCount);
            close(fd);
            return -timeoutCount;
        }
        ws_delayms(1);
    }

    //发送http协议头
    memset(shakeKey, 0, sizeof(shakeKey));
    ws_buildShakeKey(shakeKey);                                     // 创建握手key
    memset(httpHead, 0, sizeof(httpHead));                          // 创建协议包
    ws_buildHttpHead(ip, port, path, shakeKey, (char *)httpHead); // 组装http请求头
    send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);

#ifdef WS_DEBUG
    printf("ws_connectToServer: %dms\r\n%s\r\n", timeoutCount, httpHead);
#endif
    while (1)
    {
        memset(retBuff, 0, sizeof(retBuff));
        ret = recv(fd, retBuff, sizeof(retBuff), MSG_NOSIGNAL);
        if (ret > 0)
        {
            // 返回的是http回应信息
            if (strncmp((const char *)retBuff, "HTTP", strlen("HTTP")) == 0)
            {
#ifdef WS_DEBUG
                //显示http返回
                printf("ws_connectToServer: %d / %dms\r\n%s\r\n", ret, timeoutCount, retBuff);
#endif
                // 定位到握手字符串
                if ((p = strstr((char *)retBuff, "Sec-WebSocket-Accept: ")) != NULL)
                {
                    p += strlen("Sec-WebSocket-Accept: ");
                    sscanf((const char *)p, "%s\r\n", p);
                    // 比对握手信息
                    if (ws_matchShakeKey(shakeKey, strlen((const char *)shakeKey), p, strlen((const char *)p)) == 0)
                        return fd;
                    // 握手信号不对, 重发协议包
                    else
                        ret = send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);
                }
                // 重发协议包
                else
                    ret = send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);
            }
            // 显示异常返回数据
            else
            {
                // #ifdef WS_DEBUG
                if (retBuff[0] >= ' ' && retBuff[0] <= '~')
                    printf("ws_connectToServer: %d\r\n%s\r\n", ret, retBuff);
                else
                {
                    printf("ws_connectToServer: %d\r\n", ret);
                    for (i = 0; i < ret; i++)
                        printf("%.2X ", retBuff[i]);
                    printf("\r\n");
                }
                // #endif
            }
        }
        else if (ret <= 0)
            ;
        if (++timeoutCount > timeoutMs * 2)
        {
            close(fd);
            return -timeoutCount;
        }
        ws_delayms(1);
    }

    close(fd);
    return -timeoutCount;
}

/*******************************************************************************
 * 名称: ws_responseClient
 * 功能: 服务器回复客户端的连接请求, 以建立websocket连接
 * 参数:
 *      fd：连接控制符
 *      recvBuf: 接收到来自客户端的数据(内含http连接请求)
 *      bufLen: 
 * 返回: >0 建立websocket连接成功 <=0 建立websocket连接失败
 * 说明: 无
 ******************************************************************************/
int ws_responseClient(int fd, char *recvBuf, int bufLen)
{
    char *p;
    int32_t ret;
    char recvShakeKey[512] = {0};
    char respondPackage[1024] = {0};
    //获取握手key
    if ((p = strstr((char *)recvBuf, "Sec-WebSocket-Key: ")) == NULL)
        return -1;
    //获取握手key
    p += strlen("Sec-WebSocket-Key: ");
    sscanf((const char *)p, "%s", recvShakeKey);
    ret = strlen((const char *)recvShakeKey);
    if (ret < 1)
        return -1;
    //创建回复key
    ws_buildHttpRespond(recvShakeKey, (uint32_t)ret, respondPackage);
    return send(fd, respondPackage, strlen((const char *)respondPackage), MSG_NOSIGNAL);
}

/*******************************************************************************
 * 名称: ws_send
 * 功能: websocket数据基本打包和发送
 * 参数:
 *      fd：连接控制符
 *      *data: 数据
 *      dataLen: 长度
 *      mask: 数据是否使用掩码, 客户端到服务器必须使用掩码模式
 *      type: 数据要要以什么识别头类型发送(txt, bin, ping, pong ...)
 * 返回: 调用send的返回
 * 说明: 无
 ******************************************************************************/
int ws_send(int fd, char *data, int dataLen, bool mask, WsData_Type type)
{
    unsigned char *wsPkg = NULL;
    int32_t retLen, ret;
#ifdef WS_DEBUG
    uint32_t i;
    printf("ws_send: %d\r\n", dataLen);
#endif
    //数据打包 +16 预留类型、掩码、长度保存位
    wsPkg = (unsigned char *)calloc(dataLen + 16, sizeof(unsigned char));
    retLen = ws_enPackage((unsigned char *)data, dataLen, wsPkg, (dataLen + 16), mask, type);
#ifdef WS_DEBUG
    //显示数据
    printf("ws_send: %d\r\n", retLen);
    for (i = 0; i < retLen; i++)
        printf("%.2X ", wsPkg[i]);
    printf("\r\n");
#endif
    ret = send(fd, wsPkg, retLen, MSG_NOSIGNAL);
    free(wsPkg);
    return ret;
}

/*******************************************************************************
 * 名称: ws_recv
 * 功能: websocket数据接收和基本解包
 * 参数: 
 *      fd：连接控制符
 *      data: 数据接收地址
 *      dataMaxLen: 接收区可用最大长度
 * 返回: = 0 没有收到有效数据 > 0 成功接收并解包数据 < 0 非包数据的长度
 * 说明: 无
 ******************************************************************************/
int ws_recv(int fd, char *data, int dataMaxLen, WsData_Type *dataType)
{
    unsigned char *wsPkg = NULL;
    unsigned char *recvBuf = NULL;
    int32_t ret, retTemp;
    uint32_t retLen = 0;
    uint32_t retHeadLen = 0;
    int32_t retFinal = 0;
    WsData_Type retPkgType = WDT_NULL;
    //续传时,等待下一包需加时间限制
    int32_t timeoutCount = 0;

    recvBuf = (unsigned char *)calloc(dataMaxLen, sizeof(unsigned char));
    ret = recv(fd, recvBuf, dataMaxLen, MSG_NOSIGNAL);
    //数据可能超出了范围限制
    if (ret == dataMaxLen)
        printf("ws_recv: warning !! buff len %d > %d\r\n", ret, dataMaxLen);
    if (ret > 0)
    {
        //数据解包
        wsPkg = (unsigned char *)calloc(ret + 16, sizeof(unsigned char));
        retPkgType = ws_dePackage(recvBuf, ret, wsPkg, (ret + 16), &retLen, &retHeadLen);
        //非包数据, 拷贝后返回 -len
        if (retPkgType == WDT_ERR && retLen == 0)
        {
            memset(data, 0, dataMaxLen);
            if (ret < dataMaxLen)
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
        //正常包
        else
        {
            //数据可能超出了范围限制
            if (retLen > dataMaxLen)
            {
                printf("ws_recv: warning !! buff len %d > %d\r\n", retLen, dataMaxLen);
                goto recv_return_null;
            }
#ifdef WS_DEBUG
            //显示数据包的头10个字节
            if (ret > 10)
                printf("ws_recv: ret/%d, retPkgType/%d, retLen/%d, head/%d: %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X\r\n",
                       ret, retPkgType, retLen, retHeadLen,
                       recvBuf[0], recvBuf[1], recvBuf[2], recvBuf[3], recvBuf[4],
                       recvBuf[5], recvBuf[6], recvBuf[7], recvBuf[8], recvBuf[9]);
#endif
            //续传? 检查数据包的头10个字节发现recv()时并没有把一包数据接收完,继续接收..
            if (ret < retHeadLen + retLen)
            {
                timeoutCount = 50; //50*10=500ms等待
                while (ret < retHeadLen + retLen)
                {
                    ws_delayms(10);
                    retTemp = recv(fd, &recvBuf[ret], dataMaxLen - ret, MSG_NOSIGNAL);
                    if (retTemp > 0)
                    {
                        timeoutCount = 50; //50*10=500ms等待
                        ret += retTemp;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EINTR)
                            ; //连接中断
                        else
                            goto recv_return_null;
                    }
                    if (--timeoutCount < 1)
                        goto recv_return_null;
                }
                //再解包一次
                free(wsPkg);
                wsPkg = (unsigned char *)calloc(ret + 16, sizeof(unsigned char));
                retPkgType = ws_dePackage(recvBuf, ret, wsPkg, (ret + 16), &retLen, &retHeadLen);
#ifdef WS_DEBUG
                //显示数据包的头10个字节
                if (ret > 10)
                    printf("ws_recv: ret/%d, retPkgType/%d, retLen/%d, head/%d: %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X %.2X\r\n",
                           ret, retPkgType, retLen, retHeadLen,
                           recvBuf[0], recvBuf[1], recvBuf[2], recvBuf[3], recvBuf[4],
                           recvBuf[5], recvBuf[6], recvBuf[7], recvBuf[8], recvBuf[9]);
#endif
            }
            //一包数据终于完整的接收完了...
            if (retLen > 0)
            {
                if (retPkgType == WDT_PING)
                {
                    // 自动 ping-pong
                    ws_send(fd, (char *)wsPkg, retLen, true, WDT_PONG);
                    // 显示数据
                    printf("ws_recv: PING %d\r\n%s\r\n", retLen, wsPkg);
                }
                else if (retPkgType == WDT_PONG)
                {
                    printf("ws_recv: PONG %d\r\n%s\r\n", retLen, wsPkg);
                }
                else //if(retPkgType == WDT_TXTDATA || retPkgType == WDT_BINDATA || retPkgType == WDT_MINDATA)
                {
                    memcpy(data, wsPkg, retLen);
#ifdef WS_DEBUG
                    // 显示数据
                    if (wsPkg[0] >= ' ' && wsPkg[0] <= '~')
                        printf("\r\nws_recv: New Package StrFile retPkgType:%d/retLen:%d\r\n%s\r\n", retPkgType, retLen, wsPkg);
                    else
                    {
                        printf("\r\nws_recv: New Package BinFile retPkgType:%d/retLen:%d\r\n", retPkgType, retLen);
                        int32_t i;
                        for (i = 0; i < retLen; i++)
                            printf("%.2X ", wsPkg[i]);
                        printf("\r\n");
                    }
#endif
                }
                //返回有效数据长度
                retFinal = retLen;
            }
#ifdef WS_DEBUG
            else
            {
                // 显示非包数据
                if (recvBuf[0] >= ' ' && recvBuf[0] <= '~')
                    printf("\r\nws_recv: ret:%d/retPkgType:%d/retLen:%d\r\n%s\r\n", ret, retPkgType, retLen, recvBuf);
                else
                {
                    printf("\r\nws_recv: ret:%d/retPkgType:%d/retLen:%d\r\n%s\r\n", ret, retPkgType, retLen, recvBuf);
                    int32_t i;
                    for (i = 0; i < ret; i++)
                        printf("%.2X ", recvBuf[i]);
                    printf("\r\n");
                }
            }
#endif
        }
    }

    if (recvBuf)
        free(recvBuf);
    if (wsPkg)
        free(wsPkg);
    if (dataType)
        *dataType = retPkgType;
    return retFinal;

recv_return_null:

    if (recvBuf)
        free(recvBuf);
    if (wsPkg)
        free(wsPkg);
    if (dataType)
        *dataType = retPkgType;
    return 0;
}
