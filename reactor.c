#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>    // struct sockaddr_in
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <arpa/inet.h>

#define  TEST 1
#define  BUFFER_LENGTH  1024
#define  MAX_EPOLL_LENGTH 1024
typedef int NCALLBACK(int, int, void *);
// io ----> event  每个IO对应的事件
struct ntyevent{
    int fd;   //对应的fd
    int events;    // 触发的事件 读或写触发

    void *arg;    //参数
    int (*callback)(int fd, int events, void *arg);  // 触发时，执行的回调
    
    char buffer[BUFFER_LENGTH];
    int length;   //buff 实际长度

    int status;   // 状态表示是否在reator中
};

struct ntyreactor{
    int epfd;
    struct  ntyevent *events;   // 事件数组指针
};

// 将事件增加到ntyreator中
int nty_event_add(int epfd, int events, struct ntyevent *ev)
{
    struct epoll_event ep_ev = {0};

    ep_ev.data.ptr = ev;
    ep_ev.events = ev->events = events;

    int op = 0;
    // 判断该事件是否已经在reator中
    if(ev->status == 1){
        op = EPOLL_CTL_MOD;
    }else{
        op = EPOLL_CTL_ADD;
        ev->status = 1;
    }
    if(epoll_ctl(epfd, op, ev->fd, &ep_ev) < 0) {
        return -1;
    }
    return 0;
}
//删除事件events
int nty_event_del(int epfd, struct ntyevent *ev)
{
    struct epoll_event ep_ev = {0};

    if(ev->status != 1){
        return -1;
    }

    ep_ev.data.ptr = ev;
    ev->status = 0;
    if(epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &ep_ev) < 0){
        return -1;
    }
    return 0;
}

// 设置event----初始化
int nty_event_set(struct ntyevent *ev, int fd, NCALLBACK cb, void *arg)
{
    ev->fd = fd;
    ev->callback = cb;
    ev->arg = arg;

    return 0;
}

int readCallback(int fd, int events, void *arg);
int writeCallback(int fd, int events, void *arg)
{
    struct ntyreactor *reactor = (struct ntyreactor *)arg;
    if(reactor == NULL) return -1;

    struct ntyevent *ev = &reactor->events[fd];

    int len = send(fd, ev->buffer, ev->length, 0);
    if(len > 0){
        printf("send[fd=%d], [%d]%s\n", fd, len, ev->buffer);

        nty_event_del(reactor->epfd, ev);
        nty_event_set(ev, fd, readCallback, reactor);
        nty_event_add(reactor->epfd, EPOLLIN | EPOLLET, ev);
    }else{
        close(ev->fd);
        nty_event_del(reactor->epfd, ev);
        printf("send[fd=%d] error %s\n", fd, strerror(errno));
    }
    return len;
}

int readCallback(int fd, int events, void *arg)
{
    struct ntyreactor *reactor = (struct ntyreactor *)arg;
    if(reactor == NULL) return -1;

    struct ntyevent *ev = &reactor->events[fd];
    int len = recv(fd, ev->buffer, BUFFER_LENGTH, 0);
    nty_event_del(reactor->epfd, ev);

    if(len > 0){
        ev->length = len;
        ev->buffer[len] = '\0';

        printf("recv : %s\n", ev->buffer);
        nty_event_set(ev, fd, writeCallback, reactor);
        nty_event_add(reactor->epfd, EPOLLOUT, ev);
    }else if(len == 0){
        close(ev->fd);
        printf("[fd=%d] closed\n", ev->fd);
    }else {
		close(ev->fd);
		printf("recv[fd=%d] error[%d]:%s\n", fd, errno, strerror(errno));
	}
    return len;
}

int accept_callback(int fd, int evnets, void *arg)
{
    struct ntyreactor *reactor = (struct ntyreactor *)arg;

    if(reactor == NULL) return -1;

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int clientfd = accept(fd, (struct sockaddr *)&client_addr, &len);
    if(clientfd < 0) return -1;
    do{
        int i = 0;
        for (i = 0;i < MAX_EPOLL_LENGTH;i ++) {
            if (reactor->events[i].status == 0) {
                break;
            }
        }
        if (i == MAX_EPOLL_LENGTH) {
            printf("%s: max connect limit[%d]\n", __func__, MAX_EPOLL_LENGTH);
            break;
        }
         int old = fcntl(clientfd, F_GETFL);
         int ret = fcntl(clientfd, F_SETFL, O_NONBLOCK | old);
         if(ret < 0){
            printf("fcntl error\n");
            return -1;
         }
         nty_event_set(&reactor->events[clientfd], clientfd, readCallback, reactor);
         nty_event_add(reactor->epfd, EPOLLIN | EPOLLET, &reactor->events[clientfd]);
    }while(0);
    //设置fd无阻塞
   
    printf("accept fd: %d [%s : %d]\n", clientfd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    return 0;
}

// listen后添加到epoll中的事件回调
int ntyreactor_addlistener(struct ntyreactor *reactor, NCALLBACK accpetor, int sockfd)
{
    nty_event_set(&reactor->events[sockfd], sockfd, accpetor, reactor);
    // 默认为LT模式，有数据就会一直触发
    nty_event_add(reactor->epfd, EPOLLIN, &reactor->events[sockfd]);
    return 0;
}
//运行
int ntyreactor_run(struct ntyreactor *reactor)
{
    struct epoll_event events[MAX_EPOLL_LENGTH];
    int i = 0;
    while(1){
        int nready = epoll_wait(reactor->epfd, events, MAX_EPOLL_LENGTH, -1);
        if(nready < 0){
            printf("epoll_wait failed\n");
            return -1;
        }
        
        for(i = 0; i < nready; i++){
            // add -- 加入了参数  read
            struct ntyevent *ev = (struct ntyevent *)events[i].data.ptr;   // void *  -->
            if((ev->events & EPOLLIN) && (events[i].events & EPOLLIN)){
                ev->callback(ev->fd, events[i].events, ev->arg);
            }
            // write
            if((ev->events & EPOLLOUT) && (events[i].events & EPOLLOUT)){
                ev->callback(ev->fd, events[i].events, ev->arg);
            }


        }
    }
    return 0;
}

int ntyreactor_init(struct ntyreactor *reactor)
{
    if(reactor == NULL) return -1;
    memset(reactor, 0, sizeof(struct ntyreactor));

    int epfd = epoll_create(1);
    if(epfd < 0){
        return -2;
    }
    reactor->epfd = epfd;
    struct  ntyevent *events = (struct  ntyevent *)malloc(MAX_EPOLL_LENGTH * sizeof(struct ntyevent));
    if(events == NULL){
        return -3;
    }
    reactor->events = events;

    return 0;
}

int ntyreactor_destory(struct ntyreactor *reactor)
{
    if(reactor){
        close(reactor->epfd);
        free(reactor->events);
    }
    return 0;
}

int init_sock(short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        printf("socket failed\n");
        return -1;
    }
    //设置fd无阻塞
    int old = fcntl(fd, F_GETFL);
    int ret = fcntl(fd, F_SETFL, O_NONBLOCK | old);
    if(ret < 0){
        printf("fcntl error\n");
        return -1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);   //s_addr == uint32   0  绑定0为任何地址
    if(bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        printf("bind error\n");
        return -1;
    }

    if(listen(fd, 10) < 0){
        printf("listen failed : %s\n", strerror(errno));
        return -1;
    }
    printf("listen success.\n");
    return fd;
}

#if TEST

int main(int argc, char *argv[])
{
    unsigned short port = 9999;

    if(argc == 2) port = atoi(argv[1]);
    int sockfd = init_sock(port);

    struct ntyreactor *reactor = (struct ntyreactor *)malloc(sizeof(struct ntyreactor));
    ntyreactor_init(reactor);

    ntyreactor_addlistener(reactor, accept_callback, sockfd);
    ntyreactor_run(reactor);
    ntyreactor_destory(reactor);

    close(sockfd);
    return 0;
}
#endif
