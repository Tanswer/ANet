#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "define.h"
#include "ae.h"

typedef struct aeApiState {
    int epfd;  //epfd epoll事件的文件描述符
    struct epoll_event *events;  
    //事件表,每个事件对应epoll_ctl的第四个参数,定义自己的事件类型，比如EPOLLIN
} aeApiState;  //事件的状态

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) {
        goto err;
    }
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events) {
       goto err;
    }
    state->epfd = epoll_create(1024);  /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        goto err;
    }
    eventLoop->apidata = state;
    
    return 0;

err:
    if (state) {
        zfree(state->events);
        zfree(state);
    }

    return -1;
}


//调整事件表的大小
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);

    return 0;
}

//释放epoll实例和事件表的空间
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

//往epfd标识的事件表上注册 fd 的事件
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0};    /* avoid valgrind warning */

    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) {
        ee.events |= EPOLLIN;
    }
    if (mask & AE_WRITABLE) {
        ee.events |= EPOLLOUT;
    }

    ee.data.fd = fd;
    if (epoll_ctl(state->epfd, op, fd, &ee) == -1) {
        return -1;
    }

    return 0;
}

//在epfd标识的事件表中删除fd对应的事件
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) {
        ee.events |= EPOLLIN;
    }

    if (mask & AE_WRITABLE) {
        ee.events |= EPOLLOUT;
    }

    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

//等待所监听文件描述符上有事件发生
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize,
                        tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1);
    //至少有一个就绪的事件
    if (retval > 0) {
        numevents = retval;
        //遍历就绪的事件表，将其加入到eventLoop的就绪事件表中
        for (int i = 0; i < numevents; i++) {
            int mask = 0;
            struct epoll_event *e = state->events + i;

            //根据就绪的事件类型，设置mask
            if (e->events & EPOLLIN) {
                mask |= AE_READABLE;
            }
            if (e->events & EPOLLOUT) {
                mask |= AE_WRITABLE;
            }
            if (e->events & EPOLLERR) {
                mask |= AE_WRITABLE;
            }
            if (e->events & EPOLLHUP) {
                mask |= AE_WRITABLE;
            }
            //添加到就绪事件表
            eventLoop->fired[i].fd = e->data.fd;
            eventLoop->fired[i].mask = mask;
        }
    }
    //返回就绪的事件个数
    return numevents;
}

//返回正在使用的IO多路复用库的名字
static char *aeApiName(void) {
    return "epoll";
}
