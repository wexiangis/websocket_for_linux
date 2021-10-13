
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h> //使用 malloc, calloc等动态分配内存方法
#include <time.h>   //获取系统时间
#include <errno.h>
#include <pthread.h>
#include <fcntl.h> //非阻塞
#include <sys/un.h>
#include <arpa/inet.h>  //inet_addr()
#include <unistd.h>     //close()
#include <sys/types.h>  //文件IO操作
#include <sys/socket.h> //
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h> //gethostbyname, gethostbyname2, gethostbyname_r, gethostbyname_r2
#include <sys/un.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h> //SIOCSIFADDR

#include "ws_com.h"

#define WS_INFO(argv...) fprintf(stdout, "[WS_INFO] %s(%d): ", __func__, __LINE__),fprintf(stdout, ##argv)
#define WS_ERR(argv...) fprintf(stdout, "[WS_ERR] %s(%d): ", __func__, __LINE__),fprintf(stdout, ##argv)

static void WS_HEX(FILE *f, void *dat, uint32_t len)
{
    uint8_t *p = (uint8_t*)dat;
    uint32_t i;
    for (i = 0; i < len; i++)
        fprintf(f, "%02X ", p[i]);
    fprintf(f, "\r\n");
}

//稍微精准的延时
#include <sys/time.h>
void ws_delayus(uint32_t us)
{
    struct timeval tim;
    tim.tv_sec = us / 1000000;
    tim.tv_usec = us % 1000000;
    select(0, NULL, NULL, NULL, &tim);
}
void ws_delayms(uint32_t ms)
{
    ws_delayus(ms * 1000);
}

//返回时间戳字符串,格式 HH:MM:SS
char *ws_time(void)
{
    static char timeStr[9];
    struct timeval tv = {0};
    long hour;
    gettimeofday(&tv, NULL);
    hour = tv.tv_sec % 86400 / 3600 + 8;
    if (hour > 23)
        hour -= 24;
    snprintf(timeStr, sizeof(timeStr), "%02ld:%02ld:%02ld",
             hour,
             tv.tv_sec % 3600 / 60,
             tv.tv_sec % 60);
    return timeStr;
}

//==================== 域名转IP ====================

typedef struct
{
    pthread_t thread_id;
    char ip[256];
    bool result;
    bool actionEnd;
} GetHostName_Struct;

static void *ws_getHostThread(void *argv)
{
    int32_t ret;
    //int32_t i;
    char buf[1024];
    struct hostent host_body, *host = NULL;
    struct in_addr **addr_list;
    GetHostName_Struct *gs = (GetHostName_Struct *)argv;

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
    // printf("ip name: %s\r\nip list: ", host->h_name);
    // for(i = 0; addr_list[i] != NULL; i++)
    //     printf("%s, ", inet_ntoa(*addr_list[i]));
    // printf("\r\n");

    //一个域名可用解析出多个ip,这里只用了第一个
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
int ws_getIpByHostName(const char *hostName, char *retIp, int timeoutMs)
{
    int timeout = 0;
    GetHostName_Struct gs;
    if (!hostName || strlen(hostName) < 1)
        return -1;
    //开线程从域名获取IP
    memset(&gs, 0, sizeof(GetHostName_Struct));
    strcpy(gs.ip, hostName);
    gs.result = false;
    gs.actionEnd = false;
    if (pthread_create(&gs.thread_id, NULL, (void *)ws_getHostThread, &gs) < 0)
        return -1;
    //等待请求结果
    do {
        ws_delayms(1);
    } while (!gs.actionEnd && ++timeout < timeoutMs);
    //pthread_cancel(gs.thread_id);
    pthread_join(gs.thread_id, NULL);
    if (!gs.result)
        return -timeout;
    //一个域名可用解析出多个ip,这里只用了第一个
    memset(retIp, 0, strlen((const char *)retIp));
    strcpy(retIp, gs.ip);
    return timeout;
}

//==================== 加密方法BASE64 ====================

//base64编/解码用的基础字符集
static const char ws_base64char[] =
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
int32_t ws_base64_encode(const uint8_t *bindata, char *base64, int32_t binlength)
{
    int32_t i, j;
    uint8_t current;
    for (i = 0, j = 0; i < binlength; i += 3)
    {
        current = (bindata[i] >> 2);
        current &= (uint8_t)0x3F;
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((uint8_t)(bindata[i] << 4)) & ((uint8_t)0x30);
        if (i + 1 >= binlength)
        {
            base64[j++] = ws_base64char[(int32_t)current];
            base64[j++] = '=';
            base64[j++] = '=';
            break;
        }
        current |= ((uint8_t)(bindata[i + 1] >> 4)) & ((uint8_t)0x0F);
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((uint8_t)(bindata[i + 1] << 2)) & ((uint8_t)0x3C);
        if (i + 2 >= binlength)
        {
            base64[j++] = ws_base64char[(int32_t)current];
            base64[j++] = '=';
            break;
        }
        current |= ((uint8_t)(bindata[i + 2] >> 6)) & ((uint8_t)0x03);
        base64[j++] = ws_base64char[(int32_t)current];
        current = ((uint8_t)bindata[i + 2]) & ((uint8_t)0x3F);
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
int32_t ws_base64_decode(const char *base64, uint8_t *bindata)
{
    int32_t i, j;
    uint8_t k;
    uint8_t temp[4];
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
        bindata[j++] = ((uint8_t)(((uint8_t)(temp[0] << 2)) & 0xFC)) |
                       ((uint8_t)((uint8_t)(temp[1] >> 4) & 0x03));
        if (base64[i + 2] == '=')
            break;
        bindata[j++] = ((uint8_t)(((uint8_t)(temp[1] << 4)) & 0xF0)) |
                       ((uint8_t)((uint8_t)(temp[2] >> 2) & 0x0F));
        if (base64[i + 3] == '=')
            break;
        bindata[j++] = ((uint8_t)(((uint8_t)(temp[2] << 6)) & 0xF0)) |
                       ((uint8_t)(temp[3] & 0x3F));
    }
    return j;
}

//==================== 加密方法 sha1哈希 ====================

typedef struct SHA1Context
{
    uint32_t Message_Digest[5];
    uint32_t Length_Low;
    uint32_t Length_High;
    uint8_t Message_Block[64];
    int32_t Message_Block_Index;
    int32_t Computed;
    int32_t Corrupted;
} SHA1Context;

#define SHA1CircularShift(bits, word) ((((word) << (bits)) & 0xFFFFFFFF) | ((word) >> (32 - (bits))))

static void SHA1ProcessMessageBlock(SHA1Context *context)
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

static void SHA1Reset(SHA1Context *context)
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

static void SHA1PadMessage(SHA1Context *context)
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

static int32_t SHA1Result(SHA1Context *context)
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

static void SHA1Input(SHA1Context *context, const char *message_array, uint32_t length)
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

static char *sha1_hash(const char *source)
{
    SHA1Context sha;
    char *buff = NULL;

    SHA1Reset(&sha);
    SHA1Input(&sha, source, strlen(source));

    if (!SHA1Result(&sha))
        WS_ERR("SHA1 ERROR: Could not compute message digest \r\n");
    else
    {
        buff = (char *)calloc(128, sizeof(char));
        sprintf(buff, "%08X%08X%08X%08X%08X",
                sha.Message_Digest[0],
                sha.Message_Digest[1],
                sha.Message_Digest[2],
                sha.Message_Digest[3],
                sha.Message_Digest[4]);
    }
    return buff;
}

//==================== websocket部分 ====================

/*******************************************************************************
 * 名称: ws_getRandomString
 * 功能: 生成随机字符串
 * 参数: 
 *      buff: 随机字符串存储到
 *      len: 生成随机字符串长度
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
static void ws_getRandomString(char *buff, uint32_t len)
{
    uint32_t i;
    uint8_t temp;
    srand((int32_t)time(0));
    for (i = 0; i < len; i++)
    {
        temp = (uint8_t)(rand() % 256);
        if (temp == 0) //随机数不要0
            temp = 128;
        buff[i] = temp;
    }
}

/*******************************************************************************
 * 名称: ws_buildShakeKey
 * 功能: client端使用随机数构建握手用的key
 * 参数: *key: 随机生成的握手key
 * 返回: key的长度
 * 说明: 无
 ******************************************************************************/
static int32_t ws_buildShakeKey(char *key)
{
    char tempKey[16] = {0};
    ws_getRandomString(tempKey, 16);
    return ws_base64_encode((const uint8_t *)tempKey, (char *)key, 16);
}

/*******************************************************************************
 * 名称: ws_buildRespondShakeKey
 * 功能: server端在接收client端的key后,构建回应用的key
 * 参数:
 *      acceptKey: 来自客户端的key字符串
 *      acceptKeyLen: 长度
 *      respondKey:  在 acceptKey 之后加上 GUID, 再sha1哈希, 再转成base64得到 respondKey
 * 返回: respondKey的长度(肯定比acceptKey要长)
 * 说明: 无
 ******************************************************************************/
static int32_t ws_buildRespondShakeKey(char *acceptKey, uint32_t acceptKeyLen, char *respondKey)
{
    char *clientKey;
    char *sha1DataTemp;
    uint8_t *sha1Data;
    int32_t i, j, sha1DataTempLen, ret;
    const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint32_t guidLen;

    if (acceptKey == NULL)
        return 0;

    guidLen = sizeof(guid);
    clientKey = (char *)calloc(acceptKeyLen + guidLen + 10, sizeof(char));
    memcpy(clientKey, acceptKey, acceptKeyLen);
    memcpy(&clientKey[acceptKeyLen], guid, guidLen);

    sha1DataTemp = sha1_hash(clientKey);
    sha1DataTempLen = strlen((const char *)sha1DataTemp);
    sha1Data = (uint8_t *)calloc(sha1DataTempLen / 2 + 1, sizeof(char));

    //把hex字符串如"12ABCDEF",转为数值数组如{0x12,0xAB,0xCD,0xEF}
    for (i = j = 0; i < sha1DataTempLen;)
    {
        if (sha1DataTemp[i] > '9')
            sha1Data[j] = (10 + sha1DataTemp[i] - 'A') << 4;
        else
            sha1Data[j] = (sha1DataTemp[i] - '0') << 4;

        i += 1;

        if (sha1DataTemp[i] > '9')
            sha1Data[j] |= (10 + sha1DataTemp[i] - 'A');
        else
            sha1Data[j] |= (sha1DataTemp[i] - '0');

        i += 1;
        j += 1;
    }

    ret = ws_base64_encode((const uint8_t *)sha1Data, (char *)respondKey, j);

    free(sha1DataTemp);
    free(sha1Data);
    free(clientKey);
    return ret;
}

/*******************************************************************************
 * 名称: ws_matchShakeKey
 * 功能: client端收到来自服务器回应的key后进行匹配,以验证握手成功
 * 参数:
 *      clientKey: client端请求握手时发给服务器的key
 *      clientKeyLen: 长度
 *      acceptKey: 服务器回应的key
 *      acceptKeyLen: 长度
 * 返回: 0 成功  -1 失败
 * 说明: 无
 ******************************************************************************/
static int32_t ws_matchShakeKey(char *clientKey, uint32_t clientKeyLen, char *acceptKey, uint32_t acceptKeyLen)
{
    int32_t retLen;
    char tempKey[256] = {0};

    retLen = ws_buildRespondShakeKey(clientKey, clientKeyLen, tempKey);
    if (retLen != acceptKeyLen)
    {
        WS_ERR("len err, clientKey[%d] != acceptKey[%d]\r\n", retLen, acceptKeyLen);
        return -1;
    }
    else if (strcmp((const char *)tempKey, (const char *)acceptKey) != 0)
    {
        WS_ERR("strcmp err, clientKey[%s -> %s] != acceptKey[%s]\r\n", clientKey, tempKey, acceptKey);
        return -1;
    }
    return 0;
}

/*******************************************************************************
 * 名称: ws_buildHttpHead
 * 功能: 构建client端连接服务器时的http协议头, 注意websocket是GET形式的
 * 参数:
 *      ip: 要连接的服务器ip字符串
 *      port: 服务器端口
 *      path: 要连接的端口地址
 *      shakeKey: 握手key, 可以由任意的16位字符串打包成base64后得到
 *      package: 存储最后打包好的内容
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
static void ws_buildHttpHead(char *ip, int32_t port, char *path, char *shakeKey, char *package)
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
 *      acceptKey: 来自client的握手key
 *      acceptKeyLen: 长度
 *      package: 存储
 * 返回: 无
 * 说明: 无
 ******************************************************************************/
static void ws_buildHttpRespond(char *acceptKey, uint32_t acceptKeyLen, char *package)
{
    const char httpDemo[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Server: Microsoft-HTTPAPI/2.0\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s\r\n\r\n"; //时间打包待续, 格式如 "Date: Tue, 20 Jun 2017 08:50:41 CST\r\n"
    time_t now;
    struct tm *tm_now;
    char timeStr[256] = {0};
    char respondShakeKey[256] = {0};
    //构建回应的握手key
    ws_buildRespondShakeKey(acceptKey, acceptKeyLen, respondShakeKey);
    //构建回应时间字符串
    time(&now);
    tm_now = localtime(&now);
    strftime(timeStr, sizeof(timeStr), "Date: %a, %d %b %Y %T %Z", tm_now);
    //组成回复信息
    sprintf(package, httpDemo, respondShakeKey, timeStr);
}

/*******************************************************************************
 * 名称: ws_enPackage
 * 功能: websocket数据收发阶段的数据打包, 通常client发server的数据都要mask(掩码)处理, 反之server到client却不用
 * 参数:
 *      data: 准备发出的数据
 *      dataLen: 长度
 *      package: 打包后存储地址
 *      packageMaxLen: 存储地址可用长度
 *      mask: 是否使用掩码     1要   0 不要
 *      type: 数据类型, 由打包后第一个字节决定, 这里默认是数据传输, 即0x81
 * 返回: 打包后的长度(会比原数据长2~14个字节不等) <=0 打包失败 
 * 说明: 无
 ******************************************************************************/
static int32_t ws_enPackage(
    uint8_t *data,
    uint32_t dataLen,
    uint8_t *package,
    uint32_t packageMaxLen,
    bool mask,
    Ws_DataType type)
{
    uint32_t i, pkgLen = 0;
    //掩码
    uint8_t maskKey[4] = {0};
    uint32_t maskCount = 0;
    //最小长度检查
    if (packageMaxLen < 2)
        return -1;
    //根据包类型设置头字节
    if (type == WDT_MINDATA)
        *package++ = 0x80;
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
    pkgLen += 1;
    //掩码位
    if (mask)
        *package = 0x80;
    //半字节记录长度
    if (dataLen < 126)
    {
        *package++ |= (dataLen & 0x7F);
        pkgLen += 1;
    }
    //2字节记录长度
    else if (dataLen < 65536)
    {
        if (packageMaxLen < 4)
            return -1;
        *package++ |= 0x7E;
        *package++ = (uint8_t)((dataLen >> 8) & 0xFF);
        *package++ = (uint8_t)((dataLen >> 0) & 0xFF);
        pkgLen += 3;
    }
    //8字节记录长度
    else
    {
        if (packageMaxLen < 10)
            return -1;
        *package++ |= 0x7F;
        *package++ = 0; //数据长度变量是 uint32_t dataLen, 暂时没有那么多数据
        *package++ = 0;
        *package++ = 0;
        *package++ = 0;
        *package++ = (uint8_t)((dataLen >> 24) & 0xFF); //到这里就够传4GB数据了
        *package++ = (uint8_t)((dataLen >> 16) & 0xFF);
        *package++ = (uint8_t)((dataLen >> 8) & 0xFF);
        *package++ = (uint8_t)((dataLen >> 0) & 0xFF);
        pkgLen += 9;
    }
    //数据使用掩码时,使用异或解码,maskKey[4]依次和数据异或运算,逻辑如下
    if (mask)
    {
        //长度不足
        if (packageMaxLen < pkgLen + dataLen + 4)
            return -1;
        //随机生成掩码
        ws_getRandomString((char *)maskKey, sizeof(maskKey));
        *package++ = maskKey[0];
        *package++ = maskKey[1];
        *package++ = maskKey[2];
        *package++ = maskKey[3];
        pkgLen += 4;
        for (i = 0, maskCount = 0; i < dataLen; i++, maskCount++)
        {
            //maskKey[4]循环使用
            if (maskCount == 4) //sizeof(maskKey))
                maskCount = 0;
            //异或运算后得到数据
            *package++ = maskKey[maskCount] ^ data[i];
        }
        pkgLen += i;
        //断尾
        *package = '\0';
    }
    //数据没使用掩码, 直接复制数据段
    else
    {
        //长度不足
        if (packageMaxLen < pkgLen + dataLen)
            return -1;
        //这种方法,data指针位置相近时拷贝异常
        // memcpy(package, data, dataLen);
        //手动拷贝
        for (i = 0; i < dataLen; i++)
            *package++ = data[i];
        pkgLen += i;
        //断尾
        *package = '\0';
    }

    return pkgLen;
}

/*******************************************************************************
 * 名称: ws_dePackage
 * 功能: websocket数据收发阶段的数据解包,通常client发server的数据都要mask(掩码)处理,反之server到client却不用
 * 参数:
 *      data: 要解包的数据,解包后的数据会覆写到这里
 *      len: 要解包的数据的长度
 *      retDataLen: 解包数据段长度信息
 *      retHeadLen: 解包头部长度信息
 *      retPkgType: 识别包类型
 * 返回:
 *      0: 格式错误,非标准数据包数据
 *      <0: 识别包但不完整(能解析类型、掩码、长度),返回缺少的数据量(负值)
 *      >0: 解包数据成功,返回数据长度,等于retDataLen
 * 说明:
 *      建议recv时先接收14字节然后解包,根据返回缺失长度再recv一次,最后再解包,这样可有效避免连包时只解析到一包的问题
 ******************************************************************************/
static int32_t ws_dePackage(
    uint8_t *data,
    uint32_t len,
    uint32_t *retDataLen,
    uint32_t *retHeadLen,
    Ws_DataType *retPkgType)
{
    uint32_t cIn, cOut;
    //包类型
    uint8_t type;
    //数据段起始位置
    uint32_t dataOffset = 2;
    //数据段长度
    uint32_t dataLen = 0;
    //掩码
    uint8_t maskKey[4] = {0};
    bool mask = false;
    uint8_t maskCount = 0;
    //数据长度过短
    if (len < 2)
        return 0;
    //解析包类型
    if ((data[0] & 0x80) == 0x80)
    {
        type = data[0] & 0x0F;
        if (type == 0x00)
            *retPkgType = WDT_MINDATA;
        else if (type == 0x01)
            *retPkgType = WDT_TXTDATA;
        else if (type == 0x02)
            *retPkgType = WDT_BINDATA;
        else if (type == 0x08)
            *retPkgType = WDT_DISCONN;
        else if (type == 0x09)
            *retPkgType = WDT_PING;
        else if (type == 0x0A)
            *retPkgType = WDT_PONG;
        else
            return 0;
    }
    else
        return 0;
    //是否掩码,及长度占用字节数
    if ((data[1] & 0x80) == 0x80)
    {
        mask = true;
        maskCount = 4;
    }
    //2字节记录长度
    dataLen = data[1] & 0x7F;
    if (dataLen == 126)
    {
        //数据长度不足以包含长度信息
        if (len < 4)
            return 0;
        //2字节记录长度
        dataLen = data[2];
        dataLen = (dataLen << 8) + data[3];
        //转储长度信息
        *retDataLen = dataLen;
        *retHeadLen = 4 + maskCount;
        //数据长度不足以包含掩码信息
        if (len < 4 + maskCount)
            return -(int32_t)(4 + maskCount + dataLen - len);
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
    else if (dataLen == 127)
    {
        //数据长度不足以包含长度信息
        if (len < 10)
            return 0;
        //使用8个字节存储长度时,前4位必须为0,装不下那么多数据...
        if (data[2] != 0 || data[3] != 0 || data[4] != 0 || data[5] != 0)
            return 0;
        //8字节记录长度
        dataLen = data[6];
        dataLen = (dataLen << 8) | data[7];
        dataLen = (dataLen << 8) | data[8];
        dataLen = (dataLen << 8) | data[9];
        //转储长度信息
        *retDataLen = dataLen;
        *retHeadLen = 10 + maskCount;
        //数据长度不足以包含掩码信息
        if (len < 10 + maskCount)
            return -(int32_t)(10 + maskCount + dataLen - len);
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
        //转储长度信息
        *retDataLen = dataLen;
        *retHeadLen = 2 + maskCount;
        //数据长度不足
        if (len < 2 + maskCount)
            return -(int32_t)(2 + maskCount + dataLen - len);
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
    //数据长度不足以包含完整数据段
    if (len < dataLen + dataOffset)
        return -(int32_t)(dataLen + dataOffset - len);
    //解包数据使用掩码时, 使用异或解码, maskKey[4]依次和数据异或运算, 逻辑如下
    if (mask)
    {
        cIn = dataOffset;
        cOut = 0;
        maskCount = 0;
        for (; cOut < dataLen; cIn++, cOut++, maskCount++)
        {
            //maskKey[4]循环使用
            if (maskCount == 4) //sizeof(maskKey))
                maskCount = 0;
            //异或运算后得到数据
            data[cOut] = maskKey[maskCount] ^ data[cIn];
        }
        //断尾
        data[cOut] = '\0';
    }
    //解包数据没使用掩码, 直接复制数据段
    else
    {
        //这种方法,data指针位置相近时拷贝异常
        // memcpy(data, &data[dataOffset], dataLen);
        //手动拷贝
        cIn = dataOffset;
        cOut = 0;
        for (; cOut < dataLen; cIn++, cOut++)
            data[cOut] = data[cIn];
        //断尾
        data[dataLen] = '\0';
    }
    //有些特殊包数据段长度可能为0,这里为区分格式错误返回,置为1
    if (dataLen == 0)
        dataLen = 1;
    return dataLen;
}

/*******************************************************************************
 * 名称: ws_connectToServer
 * 功能: 向websocket服务器发送http(携带握手key), 以和服务器构建连接, 非阻塞模式
 * 参数:
 *      ip: 服务器ip
 *      port: 服务器端口
 *      path: 接口地址,格式如"/tmp/xxx"
 *      timeoutMs: connect阶段超时设置,接收阶段为timeoutMs*2,写0使用默认值1000
 * 返回: >0 返回连接描述符 <= 0 连接失败或超时,所花费的时间ms的负值
 * 说明: 无
 ******************************************************************************/
int ws_connectToServer(char *ip, int port, char *path, int timeoutMs)
{
    int32_t ret, fd;
    int32_t timeoutCount = 0;
    char retBuff[512] = {0};
    char httpHead[512] = {0};
    char shakeKey[128] = {0};
    char tempIp[128] = {0};
    char *p;

    //服务器端网络地址结构体
    struct sockaddr_in report_addr;
    memset(&report_addr, 0, sizeof(report_addr)); //数据初始化--清零
    report_addr.sin_family = AF_INET;             //设置为IP通信
    report_addr.sin_port = htons(port);           //服务器端口号

    //服务器IP地址, 自动域名转换
    //report_addr.sin_addr.s_addr = inet_addr(ip);
    if ((report_addr.sin_addr.s_addr = inet_addr(ip)) == INADDR_NONE)
    {
        ret = ws_getIpByHostName(ip, tempIp, 1000);
        if (ret < 0)
            return ret;
        else if (strlen((const char *)tempIp) < 7)
            return -ret;
        else
            timeoutCount += ret;
        if ((report_addr.sin_addr.s_addr = inet_addr(tempIp)) == INADDR_NONE)
            return -ret;
#ifdef WS_DEBUG
        WS_INFO("Host(%s) to IP(%s)\r\n", ip, tempIp);
#endif
    }

    //默认超时1秒
    if (timeoutMs == 0)
        timeoutMs = 1000;

    //create unix socket
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        WS_ERR("socket error\r\n");
        return -1;
    }

    //connect
    timeoutCount = 0;
    while (connect(fd, (struct sockaddr *)&report_addr, sizeof(struct sockaddr)) != 0)
    {
        if (++timeoutCount > timeoutMs)
        {
            WS_ERR("connect to %s:%d timeout(%dms)\r\n", ip, port, timeoutCount);
            close(fd);
            return -timeoutCount;
        }
        ws_delayms(1);
    }

    //非阻塞
    ret = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, ret | O_NONBLOCK);

    //发送http协议头
    memset(shakeKey, 0, sizeof(shakeKey));
    ws_buildShakeKey(shakeKey);                                   //创建握手key
    memset(httpHead, 0, sizeof(httpHead));                        //创建协议包
    ws_buildHttpHead(ip, port, path, shakeKey, (char *)httpHead); //组装http请求头
    send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);

#ifdef WS_DEBUG
    WS_INFO("connect: %dms\r\n%s\r\n", timeoutCount, httpHead);
#endif
    while (1)
    {
        memset(retBuff, 0, sizeof(retBuff));
        ret = recv(fd, retBuff, sizeof(retBuff), MSG_NOSIGNAL);
        if (ret > 0)
        {
#ifdef WS_DEBUG
            //显示http返回
            WS_INFO("recv: len %d / %dms\r\n%s\r\n", ret, timeoutCount, retBuff);
#endif
            //返回的是http回应信息
            if (strncmp((const char *)retBuff, "HTTP", 4) == 0)
            {
                //定位到握手字符串
                if ((p = strstr((char *)retBuff, "Sec-WebSocket-Accept: ")) != NULL)
                {
                    p += strlen("Sec-WebSocket-Accept: ");
                    sscanf((const char *)p, "%s\r\n", p);
                    //比对握手信息
                    if (ws_matchShakeKey(shakeKey, strlen((const char *)shakeKey), p, strlen((const char *)p)) == 0)
                        return fd;
                    //握手信号不对, 重发协议包
                    else
                        ret = send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);
                }
                //重发协议包
                else
                    ret = send(fd, httpHead, strlen((const char *)httpHead), MSG_NOSIGNAL);
            }
            //显示异常返回数据
            else
            {
                //#ifdef WS_DEBUG
                WS_ERR("recv: len %d / unknown context\r\n%s\r\n", ret, retBuff);
                WS_HEX(stderr, retBuff, ret);
                //#endif
            }
        }
        ws_delayms(1);
        //超时检查
        if (++timeoutCount > timeoutMs * 2)
            break;
    }
    //连接失败,返回耗时(负值)
    close(fd);
    return -timeoutCount;
}

/*******************************************************************************
 * 名称: ws_responseClient
 * 功能: 服务器回复客户端的连接请求, 以建立websocket连接
 * 参数:
 *      fd: 连接描述符
 *      data: 接收到来自客户端的数据(内含http连接请求)
 *      dataLen: 
 *      path: path匹配检查,不用可以置NULL
 * 返回: >0 建立websocket连接成功 <=0 建立websocket连接失败
 * 说明: 无
 ******************************************************************************/
int ws_responseClient(int fd, char *data, int dataLen, char *path)
{
    char *keyOffset;
    int32_t ret;
    char recvShakeKey[512] = {0};
    char respondPackage[1024] = {0};
#ifdef WS_DEBUG
    WS_INFO("recv: len %d \r\n%s\r\n", dataLen, data);
#endif
    //path检查
    if (path && !strstr((char *)data, path))
    {
        WS_ERR("path not matched\r\n");
        return -1;
    }
    //获取握手key
    if (!(keyOffset = strstr((char *)data, "Sec-WebSocket-Key: ")))
    {
        WS_ERR("Sec-WebSocket-Key not found\r\n");
        return -1;
    }
    //获取握手key
    keyOffset += strlen("Sec-WebSocket-Key: ");
    sscanf((const char *)keyOffset, "%s", recvShakeKey);
    ret = strlen((const char *)recvShakeKey);
    if (ret < 1)
    {
        WS_ERR("Sec-WebSocket-Key not matched\r\n");
        return -1;
    }
    //创建回复key
    ws_buildHttpRespond(recvShakeKey, (uint32_t)ret, respondPackage);
    return send(fd, respondPackage, strlen((const char *)respondPackage), MSG_NOSIGNAL);
}

/*******************************************************************************
 * 名称: ws_send
 * 功能: websocket数据基本打包和发送
 * 参数:
 *      fd: 连接描述符
 *      *data: 数据
 *      dataLen: 长度
 *      mask: 数据是否使用掩码, 客户端到服务器必须使用掩码模式
 *      type: 数据要要以什么识别头类型发送(txt, bin, ping, pong ...)
 * 返回: 调用send的返回
 * 说明: 无
 ******************************************************************************/
int ws_send(int fd, char *data, int dataLen, bool mask, Ws_DataType type)
{
    uint8_t *wsPkg = NULL;
    int32_t retLen, ret;
    //参数检查
    if (dataLen < 0)
        return 0;
    //非包数据发送
    if (type == WDT_NULL)
        return send(fd, data, dataLen, MSG_NOSIGNAL);
    //数据打包 +14 预留类型、掩码、长度保存位
    wsPkg = (uint8_t *)calloc(dataLen + 14, sizeof(uint8_t));
    retLen = ws_enPackage((uint8_t *)data, dataLen, wsPkg, (dataLen + 14), mask, type);
    if (retLen <= 0)
    {
        free(wsPkg);
        return 0;
    }
#ifdef WS_DEBUG
    //显示数据
    WS_INFO("ws_send: len/%d\r\n", retLen);
    WS_HEX(stdout, wsPkg, retLen);
#endif
    ret = send(fd, wsPkg, retLen, MSG_NOSIGNAL);
    free(wsPkg);
    return ret;
}

/*******************************************************************************
 * 名称: ws_recv
 * 功能: websocket数据接收和基本解包
 * 参数: 
 *      fd: 连接描述符
 *      data: 数据接收地址
 *      dataMaxLen: 接收区可用最大长度,至少16字节
 * 返回:
 *      =0 没有收到有效数据(或者收到特殊包,如果是 WDT_DISCONN 则fd已被close)
 *      >0 成功接收并解包数据
 *      <0 非标准数据包数据的长度
 * 说明: 无
 ******************************************************************************/
int ws_recv(int fd, char *data, int dataMaxLen, Ws_DataType *dataType)
{
    int32_t ret;
    int32_t retRecv = 0, retDePkg;
    uint32_t retDataLen = 0;
    uint32_t retHeadLen = 0;
    int32_t retFinal = 0;
    uint32_t timeout = 0;
    Ws_DataType retPkgType = WDT_NULL;
    char tmp[16];

    //丢弃数据
    if (!data || dataMaxLen < 1)
    {
        while (recv(fd, tmp, sizeof(tmp), MSG_NOSIGNAL) > 0)
            ;
    }
    //先接收数据头部,头部最大2+4+8=14字节
    else
    {
        if (dataMaxLen < 16)
            WS_ERR("error, dataMaxLen must be >= 16 \r\n");
        else
            retRecv = recv(fd, data, 14, MSG_NOSIGNAL);
    }
    if (retRecv > 0)
    {
        //数据解包
        retDePkg = ws_dePackage((uint8_t *)data, retRecv, &retDataLen, &retHeadLen, &retPkgType);
        //1. 非标准数据包数据,再接收一次(防止丢数据),之后返回 -len
        //2. dataMaxLen不足已收下这一包数据,当作非标准数据包数据处理,能收多少算多少
        if (retDePkg == 0 || (retDePkg < 0 && retRecv - retDePkg > dataMaxLen))
        {
            //能收多少算多少
            retRecv += recv(fd, &data[retRecv], dataMaxLen - retRecv, MSG_NOSIGNAL);
            //对于包过大的问题
            if (retDePkg < 0)
            {
                //1. 发出警告
                WS_ERR("warnning, pkgLen(%d) > dataMaxLen(%d)\r\n", retRecv - retDePkg, dataMaxLen);
                //2. 把这包数据丢弃,以免影响后续包
                while (recv(fd, tmp, sizeof(tmp), MSG_NOSIGNAL) > 0)
                    ;
            }
            retFinal = -retRecv;
#ifdef WS_DEBUG
            //显示数据
            WS_INFO("ws_recv1: len/%d retDePkg/%d retDataLen/%d retHeadLen/%d retPkgType/%d\r\n",
                    retRecv, retDePkg, retDataLen, retHeadLen, retPkgType);
            WS_HEX(stdout, data, retRecv);
#endif
        }
        //正常收包
        else
        {
            //检查是否需要续传
            if (retDePkg < 0)
            {
                //再接收一次(通常情况)
                ret = recv(fd, &data[retRecv], -retDePkg, MSG_NOSIGNAL);
                if (ret > 0)
                {
                    retRecv += ret;
                    retDePkg += ret;
                }
                //数据量上百K时需要多次recv,无数据200ms超时,继续接收
                for (timeout = 0; timeout < 200 && retDePkg < 0;)
                {
                    ws_delayms(5);
                    timeout += 5;
                    ret = recv(fd, &data[retRecv], -retDePkg, MSG_NOSIGNAL);
                    if (ret > 0)
                    {
                        timeout = 0;
                        retRecv += ret;
                        retDePkg += ret;
                    }
                }
#ifdef WS_DEBUG
                //显示数据
                WS_INFO("ws_recv2: len/%d retDePkg/%d retDataLen/%d retHeadLen/%d retPkgType/%d\r\n",
                        retRecv, retDePkg, retDataLen, retHeadLen, retPkgType);
                WS_HEX(stdout, data, retRecv);
#endif
                //二次解包
                retDePkg = ws_dePackage((uint8_t *)data, retRecv, &retDataLen, &retHeadLen, &retPkgType);
            }
#ifdef WS_DEBUG
            //显示数据
            WS_INFO("ws_recv3: len/%d retDePkg/%d retDataLen/%d retHeadLen/%d retPkgType/%d\r\n",
                    retRecv, retDePkg, retDataLen, retHeadLen, retPkgType);
            WS_HEX(stdout, data, retRecv);
#endif
            //一包数据终于完整的接收完了...
            if (retDePkg > 0)
            {
                //收到 PING 包,应自动回复 PONG
                if (retPkgType == WDT_PING)
                {
                    //自动 ping-pong
                    ws_send(fd, NULL, 0, false, WDT_PONG);
                    // WS_INFO("ws_recv: WDT_PING\r\n");
                    retFinal = 0;
                }
                //收到 PONG 包
                else if (retPkgType == WDT_PONG)
                {
                    // WS_INFO("ws_recv: WDT_PONG\r\n");
                    retFinal = 0;
                }
                //收到 断连 包
                else if (retPkgType == WDT_DISCONN)
                {
                    // WS_INFO("ws_recv: WDT_DISCONN\r\n");
                    retFinal = 0;
                }
                //其它正常数据包
                else
                    retFinal = retDePkg;
            }
            //未曾设想的道路...
            else
                retFinal = -retRecv;
        }
    }
    //返回包类型
    if (dataType)
        *dataType = retPkgType;

    return retFinal;
}
