#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"

#include "blue5_proxy.h"

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS		14
#endif

/*----------------------------------------------------------------------------*/
static int num_cores;
static int num_cores_used;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;


//void XorPacketPayload(char *buf, int buf_size) {
//    for (int i = 0; i < buf_size; i++) {
//        buf[i] = buf[i] ^ 0x01;
//    }
//}

void
RegisterEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    TRACE_APP_CUSTOM_DEBUG("Register new event %d in epoll descriptor for fd: %d\n", events, sock);
    int ret;

    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = sock;

#if USE_FFSTACK
    ret = ff_epoll_ctl(ctx->ep, EPOLL_CTL_ADD, sock, &ev);
#else
    ret = epoll_ctl(ctx->ep, EPOLL_CTL_ADD, sock, &ev);
#endif

    if (ret < 0 && errno != EEXIST) {
        TRACE_APP_CUSTOM_DEBUG("epoll_ctl() with EPOLL_CTL_ADD error (errno = %d) (epoll_fd = %d)\n", errno, ctx->ep);
        fprintf(stderr, "Error on ff_epoll_ctl() %s\n", strerror(errno));
        exit(-1);
    }
}

void
ModifyEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    TRACE_APP_CUSTOM_DEBUG("Modify event %d in epoll descriptor for fd: %d\n", events, sock);
    int ret;

    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = sock;

#if USE_FFSTACK
    ret = ff_epoll_ctl(ctx->ep, EPOLL_CTL_MOD, sock, &ev);
#else
    ret = epoll_ctl(ctx->ep, 3, sock, &ev);
#endif
    if (ret < 0 && errno != EEXIST) {
        TRACE_APP_CUSTOM_DEBUG("epoll_ctl() with EPOLL_CTL_MOD error (errno = %d) (epoll_fd = %d)\n", errno, ctx->ep);
        fprintf(stderr, "Error on ff_epoll_ctl() %s\n", strerror(errno));
        exit(-1);
    }
}

void
UnregisterEvent(struct thread_context *ctx, int sock)
{
    TRACE_APP_CUSTOM_DEBUG("Unregister event from epoll descriptor for fd: %d\n", sock);
    int ret;
#if USE_FFSTACK
    ret = ff_epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sock, NULL);
#else
    ret = epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sock, NULL);
#endif
    if (ret < 0 && errno != EEXIST) {
        //		TRACE_APP_CUSTOM_DEBUG("epoll_ctl() with EPOLL_CTL_DEL error\n");
        //		exit(-1);
    }
}

static void
FreeBuffer(struct thread_context *ctx, struct stream_buf *buf)
{
    if (buf == NULL)
        return;

    /* add it to the free list, only if nobody uses this buffer */
    buf->cnt_refs--;

    if (buf->cnt_refs == 0) {
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, buf, link);
        buf->data_len = 0;
    }
}

/*----------------------------------------------------------------------------*/
void
CloseConnection(struct thread_context *ctx, int sockid) {
    struct tcp_stream* hs = &ctx->tcp_streams[sockid];

    TRACE_APP_CUSTOM_DEBUG("Closing connection with sock id: %d\n", sockid);

    UnregisterEvent(ctx, sockid);

#if USE_FFSTACK
    ff_close(sockid);
#else
    close(sockid);
#endif

    FreeBuffer(ctx, hs->rbuf);
    FreeBuffer(ctx, hs->wbuf);

    if (hs->endpoint_sock >= 0) {
        ctx->tcp_streams[hs->endpoint_sock].endpoint_sock = -1;
    }
}

/*----------------------------------------------------------------------------*/
static int
CreateBackendConnection(struct thread_context *ctx, int frontend_sock) {
    struct sockaddr_in backend_addr;
    tcp_stream *backend_stream;
    int backend_fd;
    int ret;

    TRACE_APP_CUSTOM_DEBUG("Creating new backend connection\n");

#if USE_FFSTACK
    backend_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
#else
    backend_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (backend_fd < 0) {
        TRACE_APP_CUSTOM_DEBUG("error when creating a socket");
        return -1;
    }

//    if (backend_fd >= MAX_FLOW_NUM) {
//        TRACE_APP_CUSTOM_DEBUG("invalid socket id %d.\n", backend_fd);
//        return -1;
//    }

#if USE_FFSTACK
    int on = 1;
    ff_ioctl(backend_fd, FIONBIO, &on);
    ret = 0;
#else
    ret = fcntl(backend_fd, F_SETFL, O_NONBLOCK);
#endif

    if (ret < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    bzero(&backend_addr, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(8080);
    backend_addr.sin_addr.s_addr = inet_addr("10.0.1.2");

#if USE_FFSTACK
    ret = ff_connect(backend_fd, (struct linux_sockaddr*) &backend_addr, sizeof(backend_addr));
#else
    ret = connect(backend_fd, (struct sockaddr*) &backend_addr, sizeof(backend_addr));
#endif

    if (ret < 0 && errno != EINPROGRESS) {
        perror("mtcp_connect");
#if USE_FFSTACK
        ff_close(backend_fd);
#else
        close(backend_fd);
#endif
        return -1;
    }

    /* record the socket number of peer TCP stream */
    ctx->tcp_streams[frontend_sock].endpoint_sock = backend_fd;

    backend_stream = &ctx->tcp_streams[backend_fd];
    memset(backend_stream, 0, sizeof(tcp_stream));
    backend_stream->sock_id = backend_fd;
    backend_stream->endpoint_sock = frontend_sock;

    /* forward from front's read buf to backend write buf */
    backend_stream->wbuf = ctx->tcp_streams[frontend_sock].rbuf;
    ctx->tcp_streams[frontend_sock].rbuf->cnt_refs++;

    backend_stream->write_blocked = TRUE;
    RegisterEvent(ctx, backend_fd, EPOLLOUT);

    TRACE_APP_CUSTOM_DEBUG("New backend connection created with socket id: %d\n", backend_fd);

    return 0;
}

/*----------------------------------------------------------------------------*/
static int
AcceptConnection(struct thread_context *ctx, int listener) {
    struct tcp_stream *t_stream;
    struct sockaddr addr;
    socklen_t addrlen;
    int c_sock, ret;

    TRACE_APP_CUSTOM("Calling accept to check if there are other connections to accept!\n");

#if USE_FFSTACK
    c_sock = ff_accept(listener, NULL, NULL);
#else
    c_sock = accept(listener, &addr, &addrlen);
#endif

    TRACE_APP_CUSTOM("New connection accepted! Socked id: %d\n", c_sock);
    if (c_sock < 0) {
        if (errno == EAGAIN) {
            TRACE_APP_CUSTOM("errno is equal to EAGAIN\n");
            return -1;
        }
        TRACE_APP_CUSTOM("Failed to accept incoming connection.\n");
        fprintf(stderr, "Error on accept() %s\n", strerror(errno));
        exit(-1);
    }

//    if (c_sock >= MAX_FLOW_NUM) {
//        TRACE_APP_CUSTOM_DEBUG("sock id (%d) exceeds the max concurrency (%d).\n",
//                    c_sock, MAX_FLOW_NUM)
//        exit(-1);
//    }

    TRACE_APP_CUSTOM("Setting socket as non-blocking!\n");

#if USE_FFSTACK
    int on = 1;
    ff_ioctl(c_sock, FIONBIO, &on);
    ret = 0;
#else
    ret = fcntl(c_sock, F_SETFL, O_NONBLOCK);
#endif

    if (ret < 0) {
        TRACE_APP_CUSTOM_DEBUG("setting socket %d nonblocking returns error\n", c_sock);
    }

    t_stream = &ctx->tcp_streams[c_sock];
    memset(t_stream, 0, sizeof(struct tcp_stream));

    t_stream->sock_id = c_sock;
    t_stream->endpoint_sock = -1;
    t_stream->is_fronted = TRUE;

    TRACE_APP_CUSTOM("Registering new input event on new socket!\n");

    RegisterEvent(ctx, c_sock, EPOLLIN);

    return c_sock;
}

void CheckOrAllocateFreeBuffer(struct thread_context *ctx, struct tcp_stream *stream) {
    if (stream->rbuf == NULL) {
        TRACE_APP_CUSTOM_DEBUG("Allocate new read buffer\n");
        stream->rbuf = TAILQ_FIRST(&ctx->free_hbmap);
        if (!stream->rbuf) {
            fprintf(stderr, "alloc from free_hbmap fails\n");
            exit(-1);
        }
        TAILQ_REMOVE(&ctx->free_hbmap, stream->rbuf, link);

        /* (for safety) check if the given buffer is being used or has data */
        if (stream->rbuf->cnt_refs > 0) {
            fprintf(stderr, "(should not happen) there are still some refs.\n");
            exit(-1);
        }
        if (stream->rbuf->data_len > 0) {
            fprintf(stderr, "(should not happen) there are still some data.\n");
            exit(-1);
        }

        /* if there is no peer stream, it is referenced by one TCP stream */
        if (stream->endpoint_sock < 0) {
            stream->rbuf->cnt_refs = 1;
        }

            /* if there is a peer stream, it is referenced by two TCP streams */
        else {
            ctx->tcp_streams[stream->endpoint_sock].wbuf = stream->rbuf;
            stream->rbuf->cnt_refs = 2;
        }
    }

    /* make sure that it has payload buffer which is allocated during init */
    if (!stream->rbuf->data) {
        fprintf(stderr, "hs->rbuf holds a NULL buffer\n");
        exit(-1);
    }
}

//static int writen(struct thread_context *ctx, int fd, char *ptr, int nbytes)
//{
//    int nleft, nwritten;
//
//    nleft = nbytes;
//    while (nleft > 0)
//    {
//        nwritten = mtcp_write(ctx->mctx, fd, ptr, nleft);
//        if(nwritten <= 0)
//            return(nwritten);       /* error */
//
//        nleft -= nwritten;
//        ptr   += nwritten;
//    }
//    return(nbytes - nleft);
//}


static int
WriteAvailData(struct thread_context *ctx, int fd)
{
    tcp_stream *t_stream = &ctx->tcp_streams[fd];
    stream_buf *buff = t_stream->wbuf;
    int res;

//    if (buff->data_len < 1 || t_stream->write_blocked == 1) {
//        TRACE_APP_CUSTOM_DEBUG("No data to write or write blocked\n");
//        return 0;
//    }

//    if((res = writen(ctx, fd, buff->data, buff->data_len)) != buff->data_len)
//    {
//        TRACE_APP_CUSTOM_DEBUG("Error in writen!\n");
//        return -1;
//    }
#if USE_FFSTACK
    res = ff_write(fd, buff->data, buff->data_len);
#else
    res = write(fd, buff->data, buff->data_len);
#endif

    TRACE_APP_CUSTOM_DEBUG("Wrote %d bytes to sock id: %d\n", res, fd);

    if (res < 0) {
        /* we might have been full but didn't realize it */
        if (errno == EAGAIN) {
            TRACE_APP_CUSTOM_DEBUG("We might have been full but didn't realize it\n");
            t_stream->write_blocked = 1;
            ModifyEvent(ctx, fd, EPOLLOUT);
            return 0;
        }

        /* error occured while writing to remote host */
        return -1;
    }

    /* if (res > 0) */
    buff->data_len -= res;

//    if (t_stream->is_fronted) {
//        t_stream->bytes_to_write -= res;
//
//        /* mismatch cases (exit for debugging purposes now) */
//        if (t_stream->bytes_to_write < 0 ||
//            (t_stream->bytes_to_write == 0 && buff->data_len > 0)) {
//            fprintf(stderr, "content-length mismatch (bytes_to_write: %d, data_len: %d)\n",
//                    (int) t_stream->bytes_to_write, buff->data_len);
//            exit(-1);
//        }
//
//        /* finished a HTTP GET, so wait for the next connection */
//        if (t_stream->bytes_to_write == 0) {
//            if (t_stream->wbuf->data_len > 0 || t_stream->rbuf->data_len > 0) {
//                fprintf(stderr, "hs->wbuf->data_len = %d, hs->rbuf->data_len = %d\n",
//                        t_stream->wbuf->data_len, t_stream->rbuf->data_len);
//                exit(-1);
//            }
//
//            /* backend connection is already closed */
//            if (t_stream->endpoint_sock < 0) {
//                CloseConnection(ctx, fd);
//                return 0;
//            }
//
//            /* if (hs->peer_sock >= 0) */
//            /* backend server may close the connection */
//            ModifyEvent(ctx, fd, MTCP_EPOLLIN);
//            ModifyEvent(ctx, t_stream->endpoint_sock, MTCP_EPOLLIN);
//        }
//    }

    /* since we could not write all, assume that it's blocked */
    if (buff->data_len > 0) {
        TRACE_APP_CUSTOM_DEBUG("Left %d bytes to write, add new write event\n", buff->data_len);
        memmove(buff->data, &buff->data[res], buff->data_len);
        t_stream->write_blocked = 1;
        ModifyEvent(ctx, fd, EPOLLOUT);
    }

    return 0;
}

static void HandleReadEvent(struct thread_context *ctx, int fd) {
    tcp_stream *t_stream;
    int space_left, res;

    TRACE_APP_CUSTOM_DEBUG("Handle Read Event called!\n");
    // if peer is closed, close ourselves
    t_stream = &ctx->tcp_streams[fd];
    // If the backend connection has not been created (yet), the value will be 0
//    if (t_stream->endpoint_sock < 0) {
//        CloseConnection(ctx, fd);
//        return;
//    }

    TRACE_APP_CUSTOM_DEBUG("Check of allocate free buffer\n");
    /* if there is no read buffer in this stream, bring one from free list */
    CheckOrAllocateFreeBuffer(ctx, t_stream);

    if ((space_left = MAX_BUF_SIZE - t_stream->rbuf->data_len - 1) <= 0) {
        // Unregister from read event for a while */
        UnregisterEvent(ctx, fd);
        return;
    }

    TRACE_APP_CUSTOM_DEBUG("Read data from socket %d\n", fd);
#if USE_FFSTACK
    res = ff_read(fd, &t_stream->rbuf->data[t_stream->rbuf->data_len], space_left);
#else
    res = read(fd, &t_stream->rbuf->data[t_stream->rbuf->data_len], space_left);
#endif

    TRACE_APP_CUSTOM_DEBUG("Read %d byte/s from socket %d\n", res, fd);
    /* when a connection closed by remote host */
    if (res == 0) {
        CloseConnection(ctx, fd);
        if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0) {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
        return;
    }

    /* read is unavailable or an error occured */
    if (res == -1) {
        if (errno != EAGAIN) {
            TRACE_APP_CUSTOM_DEBUG("mtcp_read() error\n");
            fprintf(stderr, "Error on mtcp_read() %s\n", strerror(errno));
            CloseConnection(ctx, fd);
            if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0) {
                CloseConnection(ctx, t_stream->endpoint_sock);
                t_stream->endpoint_sock = -1;
            }
        }
        return;
    }

    /* res > 0 */
    t_stream->rbuf->data_len += res;
    t_stream->rbuf->data[t_stream->rbuf->data_len] = 0;

    if (t_stream->is_fronted) {
        /* so let's connect to the backend server */
        if (t_stream->endpoint_sock < 0) {
            /* case 1: create a new connection (or bring one from pool) */
            TRACE_APP_CUSTOM_DEBUG("Let's create a new endpoint connection\n");
            if (CreateBackendConnection(ctx, fd) < 0) {
                CloseConnection(ctx, fd);
            }
            return;
        }
        else {	/* t_stream->peer_sock >= 0 */
            /* proceed and write available data (= request) to server */
            /* (you already have a backend connetion, go ahead) */
            ModifyEvent(ctx, t_stream->endpoint_sock, EPOLLIN);
//            RegisterEvent(ctx, t_stream->endpoint_sock, EPOLLIN);
        }
    } else {
        assert(t_stream->endpoint_sock >= 0);
    }

    TRACE_APP_CUSTOM_DEBUG("Writing available data to socket %d\n", t_stream->endpoint_sock);

    /* try writing available data in the buffer including that we read */
    if (WriteAvailData(ctx, t_stream->endpoint_sock) < 0) {
        TRACE_APP_CUSTOM_DEBUG("WriteAvailData() error\n");
        /* close both side of HTTP stream */
        CloseConnection(ctx, fd);
        if (t_stream->endpoint_sock >= 0) {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
    }
}

static void
HandleWriteEvent(struct thread_context *ctx, int fd)
{
    tcp_stream *hs = &ctx->tcp_streams[fd];

    /* unblock it and read what it has */
    hs->write_blocked = FALSE;
    TRACE_APP_CUSTOM_DEBUG("Modifying write event for sock id: %d\n", fd);
//    UnregisterEvent(ctx, fd);
//    RegisterEvent(ctx, fd, EPOLLIN);
    ModifyEvent(ctx, fd, EPOLLIN);

    /* enable reading on peer just in case it was off */
    if (hs->endpoint_sock >= 0) {
        TRACE_APP_CUSTOM_DEBUG("Registering read event for sock id: %d\n", hs->endpoint_sock);
        RegisterEvent(ctx, hs->endpoint_sock, EPOLLIN);
    }

    TRACE_APP_CUSTOM_DEBUG("Writing available data to sock id: %d\n", fd);
    /* if we have data, write it */
    if (WriteAvailData(ctx, fd) < 0) {
        /* if write fails, close the HTTP stream */
        CloseConnection(ctx, fd);
        if (hs->endpoint_sock >= 0) {
            CloseConnection(ctx, hs->endpoint_sock);
            hs->endpoint_sock = -1;
        }
        return;
    }

//    RegisterEvent(ctx, fd,EPOLLIN);

    /* if peer is closed and we're done writing, we should close */
    if (hs->endpoint_sock < 0 && hs->wbuf->data_len == 0) {
        CloseConnection(ctx, fd);
    }
}


int initEpollDescriptor(struct thread_context *ctx) {
#if USE_FFSTACK
    assert((ctx->ep = ff_epoll_create(0)) > 0);
    TRACE_APP_CUSTOM_DEBUG("Create epoll fd %d\n", ctx->ep);
#else
    ctx->ep = epoll_create(MAX_EVENTS);
#endif

    if (ctx->ep < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to create epoll descriptor!\n");
        return -1;
    }

    //ctx->events = (struct epoll_event *)
    //        calloc(MAX_EVENTS, sizeof(struct epoll_event));
    memset(&ctx->events, 0, MAX_EVENTS * sizeof(struct epoll_event));

//    if (!*events) {
//        TRACE_APP_CUSTOM_DEBUG("Failed to create event struct!\n");
//        return -1;
//    }

    return 0;
}

/*----------------------------------------------------------------------------*/
static int
CreateListeningSocket(struct thread_context *ctx) {
    int listener;
    int ret;

    /* create socket and set it as nonblocking */
    listener = ff_socket(AF_INET, SOCK_STREAM, 0);


    if (listener < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to create listening socket!\n");
        return -1;
    }

    int on = 1;
    ff_ioctl(listener, FIONBIO, &on);
    ret = 0;


    if (ret < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    struct sockaddr_in my_addr;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(9090);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = ff_bind(listener, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));

    if (ret < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to bind to the listening socket!\n");
        return -1;
    }

    /* listen (backlog: can be configured) */
    ret = ff_listen(listener, backlog);

    if (ret < 0) {
        TRACE_APP_CUSTOM_DEBUG("mtcp_listen() failed!\n");
        return -1;
    }

    initEpollDescriptor(ctx);

    /* wait for incoming accept events */
    RegisterEvent(ctx, listener, EPOLLIN);

    return listener;
}

int initServerVariables(struct thread_context *ctx) {
    ctx->tcp_streams = (struct tcp_stream*)calloc(MAX_FLOW_NUM,
                                                  sizeof(struct tcp_stream));
    if (!ctx->tcp_streams) {
        return -1;
    }

    return 0;
}

int initFreeFlowBuffers(struct thread_context *ctx) {
    /* initialize memory pool for flow buffers */
    ctx->hbmap = (stream_buf*) calloc(MAX_FLOW_NUM,
                                      sizeof(struct stream_buf));

    if (!ctx->hbmap) {
        TRACE_APP_CUSTOM_DEBUG("Failed to allocate memory for flow buffer map.\n");
        return -1;
    }

    for (int i = 0; i < MAX_FLOW_NUM; i++) {
        ctx->hbmap[i].data = (char*) calloc(1, MAX_BUF_SIZE);
        if (!ctx->hbmap[i].data) {
            TRACE_APP_CUSTOM_DEBUG("Failed to allocate memory for flow buffer.\n");
            return -1;
        }
    }

    TAILQ_INIT(&ctx->free_hbmap);
    for (int i = 0; i < MAX_FLOW_NUM; i++)
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, &ctx->hbmap[i], link);

    return 0;
}

int loop (void *arg) {
    struct thread_context *ctx;
    ctx = (struct thread_context *) arg;
    int do_accept;
    int i, nevents;

    //TRACE_APP_CUSTOM_DEBUG("Waiting on epoll fd %d\n", ctx->ep);
    nevents = ff_epoll_wait(ctx->ep, ctx->events, MAX_EVENTS, -1);
//    if (nevents < 0 && errno != EINTR) {
//        if (errno == EPERM)
//            return 0;
//        TRACE_APP_CUSTOM_DEBUG("mtcp_epoll_wait() error\n");
//        exit(-1);
//    }
    //TRACE_APP_CUSTOM_DEBUG("Received new event to %d\n", nevents);
    if (nevents < 0) {
        printf("ff_epoll_wait failed:%d, %s\n", errno,
               strerror(errno));
    }

    do_accept = FALSE;
    for (i = 0; i < nevents; i++) {
        // if the event is for the listener, accept connection
            if (ctx->events[i].data.fd == ctx->listener) {
                TRACE_APP_CUSTOM_DEBUG("Received new event to accept\n");
                do_accept = TRUE;
                // when read becomes available, handle read event
            } else if (ctx->events[i].events & EPOLLIN) {
                TRACE_APP_CUSTOM_DEBUG("New READ event arrived on sock id: %d\n", ctx->events[i].data.fd);
                HandleReadEvent(ctx, ctx->events[i].data.fd);
                // when write becomes available handle write event
            } else if (ctx->events[i].events & EPOLLOUT) {
                TRACE_APP_CUSTOM_DEBUG("New WRITE event arrived on sock id: %d\n", ctx->events[i].data.fd);
                HandleWriteEvent(ctx, ctx->events[i].data.fd);
            } else if (ctx->events[i].events & EPOLLERR) {
                fprintf(stderr, "MTCP_EPOLLERR\n");
                UnregisterEvent(ctx, ctx->events[i].data.fd);
                CloseConnection(ctx, ctx->events[i].data.fd);
            } else if (ctx->events[i].events & EPOLLHUP) {
                fprintf(stderr, "MTCP_EPOLLHUP\n");
                exit(-1); /* for debugging now */
            } else if (ctx->events[i].events & EPOLLRDHUP) {
                fprintf(stderr, "MTCP_EPOLLRDHUP\n");
                exit(-1); /* for debugging now */
            }
        else {
            /* Unknown epoll flag */
            fprintf(stderr, "unknown epoll flag\n");
            exit(-1);
        }
    }

    // if do_accept flag is set, accept connections
    if (do_accept) {
        TRACE_APP_CUSTOM_DEBUG("Received new event\n");
        while (AcceptConnection(ctx, ctx->listener) >= 0);
    }
}

/*----------------------------------------------------------------------------*/
int RunMainLoop(void *arg_ctx) {
    struct thread_context *ctx;
    int ret;

    ctx = (struct thread_context *) arg_ctx;

    TRACE_APP_CUSTOM("Run application on core %d\n", ctx->cpu);

//    TRACE_APP_CUSTOM("Initialize EPOLL Descriptors!\n");
//
//    // Create epoll descriptor
//    assert((ctx->ep = ff_epoll_create(0)) > 0);
//    TRACE_APP_CUSTOM_DEBUG("Create epoll fd %d\n", ctx->ep);
//
//    if (ctx->ep < 0) {
//        TRACE_APP_CUSTOM_DEBUG("Failed to create epoll descriptor!\n");
//        return -1;
//    }
//
//    //ctx->events = (struct epoll_event *)
//    //        calloc(MAX_EVENTS, sizeof(struct epoll_event));
//    memset(&ctx->events, 0, MAX_EVENTS * sizeof(struct epoll_event));
//
//    if (ret < 0) {
//        TRACE_APP_CUSTOM("Error while initializing epoll descriptor!\n");
//        exit(-1);
//    }

    TRACE_APP_CUSTOM("Allocate memory for server variables!\n");
    // Allocate memory for server variables
    ret = initServerVariables(ctx);
    if (ret < 0) {
        TRACE_APP_CUSTOM("Failed to create server_vars struct!\n");
        exit(-1);
    }

    TRACE_APP_CUSTOM("Allocate memory for free flow buffers!\n");
    // Allocate memory for free flow buffers
    ret = initFreeFlowBuffers(ctx);
    if (ret < 0) {
        TRACE_APP_CUSTOM("Failed to allocate flow buffers!\n");
        exit(-1);
    }

    TRACE_APP_CUSTOM("Create listening socket!\n");

    ctx->listener = CreateListeningSocket(ctx);
    if (ctx->listener < 0) {
        TRACE_APP_CUSTOM_DEBUG("Failed to create listening socket.\n");
        exit(1);
    }

    ff_run(loop, (void *)ctx);
    return 0;
    //free(ctx->tcp_streams);
}

/*----------------------------------------------------------------------------*/
//void *
//RunFFStack(void *arg)
//{
//    int core = *(int *)arg;
//    struct thread_context *ctx = (struct thread_context *) calloc(1, sizeof(struct thread_context));
//    if (!ctx) {
//        TRACE_APP_CUSTOM_DEBUG("Failed to create thread context!\n");
//        exit(-1);
//    }
//
//    ctx->cpu = core;
//
//    /* run main application loop */
//    ff_run(RunMainLoop, (void *)ctx);
//
//    //free(ctx);
//
//    //pthread_exit(NULL);
//}

/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum) {
    int i;

    for (i = 0; i < core_limit; i++) {
        if (app_thread[i] == pthread_self()) {
            //TRACE_INFO("Server thread %d got SIGINT\n", i);
            done[i] = TRUE;
        } else {
            if (!done[i]) {
                pthread_kill(app_thread[i], signum);
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name) {
    TRACE_APP_CUSTOM_DEBUG("%s -p <path_to_www/> -f <mtcp_conf_file> "
                 "[-N num_cores] [-c <per-process core_id>] [-h]\n",
                 prog_name);
    exit(EXIT_SUCCESS);
}

/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv) {
    int cores[MAX_CPUS];
    int i, o;
    num_cores = MAX_CPUS;

    ff_init(argc, argv);

    /* if backlog is not specified, set it to 4K */
    if (backlog == -1) {
        backlog = 4096;
    }

    TRACE_APP_CUSTOM_DEBUG("Application initialization finished.\n");

    num_cores_used = 0;
    core_limit = 1;

    //int core = *(int *)arg;
    struct thread_context *ctx = (struct thread_context *) calloc(1, sizeof(struct thread_context));
    if (!ctx) {
        TRACE_APP_CUSTOM_DEBUG("Failed to create thread context!\n");
        exit(-1);
    }

    ctx->cpu = 0;

    RunMainLoop((void *)ctx);
    /* run main application loop */
    //ff_run(RunMainLoop, (void *)ctx);

    //RunFFStack((void *)&cores[i]);

//    for (i = 0; i < core_limit; i++) {
//        cores[i] = i;
//        num_cores_used++;
//        TRACE_APP_CUSTOM_DEBUG("Creating thread %d\n", i);
//        if (pthread_create(&app_thread[i], NULL, RunFFStack, (void *)&cores[i])) {
//            TRACE_APP_CUSTOM_DEBUG("Failed to create msg_test thread.\n");
//            exit(-1);
//        }
//    }
//
//    for (i = 0; i < num_cores_used; i++) {
//        pthread_join(app_thread[i], NULL);
//        TRACE_APP_CUSTOM_DEBUG("Message test thread %d joined.\n", i);
//    }

    return 0;
}
