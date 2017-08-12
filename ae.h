#ifndef ANET_AE_H
#define ANET_AE_H

#include <time.h>

#define AE_OK        0
#define AE_ERR      -1

#define AE_NONE      0
#define AE_READABLE  1
#define AE_WRITABLE  2

#define AE_FILE_EVENTS  1
#define AE_TIME_EVENTS  2
#define AE_ALL_EVENTS    (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT    4

#define AE_NOMORE            -1
#define AE_DELETED_EVENT_ID  -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
//回调函数，如果当前文件事件所指定的事件类型发生时，就会调用相应的回调函数处理该事件
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);

typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);

typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);

typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* 文件事件结构体   通常的套接字 对套接字的抽象 */
typedef struct aeFileEvent {
    int mask;       /* one of AE_(NONE | READABLE | WRITABLE) 事件码：可读/可写 */
    aeFileProc *rfileProc;  /* 读事件的处理函数 */
    aeFileProc *wfileProc;  /* 写事件的处理函数 */ 
    void *clientData;       /* 客户端传入的数据*/
} aeFileEvent;

/* Time event structure 定时事件结构体 */  //服务器的一些操作需要在给定的时间点执行
typedef struct aeTimeEvent {
    long long id;       /* time event identifier. 定时事件ID */
    long when_sec;      /* seconds 秒 时间事件到达的秒数 */
    long when_ms;       /* milliseconds 毫秒 时间事件到达的毫秒数 */
    aeTimeProc *timeProc;   /* 定时事件处理函数 */
    aeEventFinalizerProc *finalizerProc; /* 定时事件终结函数 */
    void *clientData;   /* 客户端传入的数据 */
    struct aeTimeEvent *next; /* 下一个节点 */
} aeTimeEvent;

/* A fired event 发生了事件|就绪事件 的结构体 epoll_wait后用*/
typedef struct aeFiredEvent {
    int fd;     /* 就绪事件的文件描述符 fd */
    int mask;   /* 就绪事件的类型:读/写 */
} aeFiredEvent;

/* State of an event based program 事件状态结构 */
typedef struct aeEventLoop {
    int maxfd;                  /* 当前注册的最大文件描述符 */
    int setsize;                /* 监控的最大文件描述符数 */
    long long timeEventNextId;  /* 定时事件ID */
    time_t lastTime;            /* 最近一次处理事件的时间 */
    aeFileEvent *events;        /* 注册的事件表 数组 */
    aeFiredEvent *fired;        /* 已就绪事件表 */
    aeTimeEvent *timeEventHead; /* 定时事件链表头节点 */
    int stop;                   /* 是否停止循环 事件处理开关 */
    void *apidata;              /* 特定接口的特定数据 保存底层调用的多路复用库的事件状态 */
    aeBeforeSleepProc *beforesleep;     /* 在sleep之前执行的函数 */
} aeEventLoop;          //事件轮询的状态结构

aeEventLoop *aeCreateEventLoop(int setsize);

void aeDeleteEventLoop(aeEventLoop *eventLoop);

void aeStop(aeEventLoop *eventLoop);

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData);

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);

int aeGetFileEvents(aeEventLoop *eventLoop, int fd);

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc);

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);

int aeProcessEvents(aeEventLoop *eventLoop, int flags);

void aeMain(aeEventLoop *eventLoop);

int aeWait(int fd, int mask, long long milliseconds);

char *aeGetApiName(void);

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);

int aeGetSetSize(aeEventLoop *eventLoop);

int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif //ANET_AE_H
