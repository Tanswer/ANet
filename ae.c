#include <stdio.h>
#include <sys/time.h>

#include <poll.h>
#include <errno.h>

#ifdef __linux__
#include "ae_epoll.c"
#else

#include "ae_select.c"

#endif

/* 给server->loop 申请空间 */
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;

    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) {  //给eventLoop申请空间
        goto err;
    }

    eventLoop->events = zmalloc(sizeof(aeFileEvent) * setsize);     //给event数组申请空间
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent) * setsize);     //给fired数组申请空间
    if (eventLoop->events == NULL || eventLoop->fired == NULL) {    //申请成功
        goto err;
    }

    eventLoop->setsize = setsize;       //设置大小
    eventLoop->lastTime = time(NULL);   //设置lastTime=NULL
    eventLoop->timeEventHead = NULL;    //定时事件链表置空
    eventLoop->timeEventNextId = 0;     //定时事件的id为0
    eventLoop->stop = 0;                //stop为0
    eventLoop->maxfd = -1;              //最大文件描述符为-1
    eventLoop->beforesleep = NULL;      //beforesleep设置为NULL

    if (aeApiCreate(eventLoop) == -1) { 
        //给epoll申请空间，创建一个epoll实例，保存到aeEventLoop中
        goto err;
    }

    /* Events with mask == AE_NONE are not set. So let's initialize the vector with it. */
    for (int i = 0; i < setsize; i++) {
        eventLoop->events[i].mask = AE_NONE; //将每一个fd的事件类型初始化为 AE_NONE 
    }

    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }

    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) {
        return AE_OK;
    }

    if (eventLoop->maxfd >= setsize) {
        return AE_ERR;
    }

    if (aeApiResize(eventLoop, setsize) == -1) {
        return AE_ERR;
    }

    eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * setsize);
    eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with an AE_NONE mask. */
    for (int i = eventLoop->maxfd + 1; i < setsize; i++) {
        eventLoop->events[i].mask = AE_NONE;
    }

    return AE_OK;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData) {
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    aeFileEvent *fe = &eventLoop->events[fd]; //利用fe指向eventLoop->events[fd]

    if (aeApiAddEvent(eventLoop, fd, mask) == -1) { 
        //往epoll添加事件，本质调用epoll_ctl(epfd,EPOLL_CTL_ADD,fd,...)
        return AE_ERR;
    }

    fe->mask |= mask; //如果fe->mask之前不是空，现在就相当于同时监控两个事件
    if (mask & AE_READABLE) {
        fe->rfileProc = proc;  //proc是读操作的处理函数
    }

    if (mask & AE_WRITABLE) {
        fe->wfileProc = proc;  //proc是写操作的处理函数
    }

    fe->clientData = clientData; //让它们指向同一个client或server实例
    if (fd > eventLoop->maxfd) {
        eventLoop->maxfd = fd;   //如果新的fd大于maxfd,则更新maxfd
    }

    return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
    if (fd >= eventLoop->setsize) {
        return;
    }

    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) {
        return;
    }

    aeApiDelEvent(eventLoop, fd, mask);

    fe->mask &= ~mask;
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;
        for (j = eventLoop->maxfd - 1; j >= 0; j--) {
            if (eventLoop->events[j].mask != AE_NONE) {
                break;
            }
        }
        eventLoop->maxfd = j;
    }
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) {
        return 0;
    }

    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

static void aeGetTime(long *seconds, long *milliseconds) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
}

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds / 1000;
    when_ms = cur_ms + milliseconds % 1000;
    if (when_ms >= 1000) {
        when_sec++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) {
        return AE_ERR;
    }

    te->id = id;
    aeAddMillisecondsToNow(milliseconds, &te->when_sec, &te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;

    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;

    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    while (te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }

    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while (te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms)) {
            nearest = te;
        }
        te = te->next;
    }

    return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te, *prev;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while (te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    eventLoop->lastTime = now;

    prev = NULL;
    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId - 1;
    while (te) {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            if (prev == NULL) {
                eventLoop->timeEventHead = te->next;
            } else {
                prev->next = te->next;
            }

            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
            }

            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec || (now_sec == te->when_sec && now_ms >= te->when_ms)) {
            int retval;

            id = te->id;
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
            } else {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        prev = te;
        te = te->next;
    }

    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */

/* Redis服务器在没有被事件触发时，就会阻塞等待，因为没有设置AE_DONT_WAIT标识
 * 但是它不会一直死等待，等待文件事件的到来，因为它还要处理时间事件
 * so 在调用aeApiPoll进行监听之前，先从时间事件链表中获取一个最近到达的时间事件
 * 根据要等待的事件构建一个struct timeval tv,*tvp结构的变量，
 * 这个变量作为epoll_wait第四个参数，保存着服务器阻塞等待文件事件的最长时间，
 * 一旦时间到达而没触发文件事件 * aeApiPoll函数就会停止阻塞，立即返回
 * 接着调用processTimeEvents处理时间事件
 * 如果阻塞等待的最长时间(也就是最近的时间事件到来)之间，触发了文件事件，
 * 就会先执行文件事件，后执行时间事件，因此处理定时事件通常比预设的晚一些*/


/* 函数返回处理的事件个数
 * flags==0  表示函数神码都不做，直接返回
 * 如果flags 设置了 AE_DONT_WAIT，那么函数处理完事件后直接返回，不阻塞等待
 * 如果flags 设置了 AE_FILE_EVENTS，则执行文件事件
 * 如果flags 设置了 AE_TIME_EVENTS，则执行定时事件
 * 如果flags 设置了 AE_ALL_EVENTS，则执行所有类型的事件
 * 在这设置的是 AE_ALL_EVENTS ,没有设置 AE_DONT_WAIT */

int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    int processed = 0, numevents;


    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) {
        //如果什么事件都没有设置则直接返回
        return 0;
    }

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. 
     * 注意，我们即使没有文件事件，因为我们要处理时间事件，
     * 我们仍然要调用select(),以便在下一次事件准备启动之前进行休眠
     * */
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        //如果有定时事件处理
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        //如果设置了定时事件，而没有设置不阻塞标识
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT)) {
            //for循环找到最近需要发生的定时事件
            shortest = aeSearchNearestTimer(eventLoop);
        }

        //如果定时事件链表不空，获取到最早到的时间事件
        if (shortest) {
            long now_sec, now_ms;
            //获取当前时间
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
 
            /* How many milliseconds we need to wait for the next time event to fire? */
            //计算我们需要等待的ms数，直到最近的定时事件发生
            long long ms = (shortest->when_sec - now_sec) * 1000 + shortest->when_ms - now_ms;

            if (ms > 0) {
                //如果定时事件没有过期，计算出需要等待的时间，
                //作为epoll_wait的第四个参数
                tvp->tv_sec = ms / 1000;
                tvp->tv_usec = (ms % 1000) * 1000;
            } else {
                //否则置为0,epoll_wait就不会阻塞
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {  
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout to zero */
            /* 此时定时事件链表为空，如果我们设置了不阻塞标识，那么将tv置为0
             * 那么就不阻塞，epoll_wait直接返回
             * 否则就一直等文件事件到来*/
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }
        //调用epoll_wait函数，返回就绪文件事件个数
        numevents = aeApiPoll(eventLoop, tvp);
        for (int i = 0; i < numevents; i++) {  //遍历依次处理loop->fired
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[i].fd];
            int mask = eventLoop->fired[i].mask;
            int fd = eventLoop->fired[i].fd;

            int rfired = 0;
            /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            //如果文件是可读事件发生
            if (fe->mask & mask & AE_READABLE) {
                rfired = 1;      //确保读或写只执行一个，此处设置读事件标识
                fe->rfileProc(eventLoop, fd, fe->clientData, mask); 
                //执行读处理
            }
            //可写事件发生
            if (fe->mask & mask & AE_WRITABLE) {
                if (!rfired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask);
                }
            }
            processed++;  //执行事件的次数加1
        }
    }
    /* Check time events */
    //执行时间事件
    if (flags & AE_TIME_EVENTS) {
        processed += processTimeEvents(eventLoop);
    }

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    bzero(&pfd, sizeof(pfd));
    pfd.fd = fd;

    if (mask & AE_READABLE) {
        pfd.events |= POLLIN;
    }

    if (mask & AE_WRITABLE) {
        pfd.events |= POLLOUT;
    }

    if ((retval = poll(&pfd, 1, (int) milliseconds)) == 1) {
        if (pfd.revents & POLLIN) {
            retmask |= AE_READABLE;
        }
        if (pfd.revents & POLLOUT) {
            retmask |= AE_WRITABLE;
        }
        if (pfd.revents & POLLERR) {
            retmask |= AE_WRITABLE;
        }
        if (pfd.revents & POLLHUP) {
            retmask |= AE_WRITABLE;
        }
        return retmask;
    }

    return retval;
}

//事件轮询的主函数
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    //一直处理事件 事件驱动的典型特征就是死循环
    while (!eventLoop->stop) {  
        if (eventLoop->beforesleep) {
            eventLoop->beforesleep(eventLoop);
        }
        //处理到时的定时事件和就绪的文件事件
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
        //AE_ALL_EVENTS定义在ae.h中，代表文件事件和时间事件
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}
