#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/eventfd.h>


#include "event.h"

#define PORT        25341
#define BACKLOG     5
#define MEM_SIZE    1024

int debug = 0;



struct event_base* base;
unsigned int g_offset = 0;



// 工作线程相关的配置文件
const unsigned int THREAD_NUMBER =  24;
struct event_base*  work_base[ THREAD_NUMBER ];
int event_fd_set [THREAD_NUMBER];
unsigned int thread_offset [THREAD_NUMBER];

int fd_used_init = 0;  // 用来回避 eventfd 的 init value， 多一次 read 的问题


#define handle_error(msg)  do { perror(msg); exit(-1); } while (0)
//#define handle_error(msg)  do { perror(msg); exit(EXIT_FAILURE); } while (0)


void * child_main(void* args)
{
    // child init
    unsigned int * p = (unsigned int *)args;
    unsigned int offset = *p; 



    while(1)
    {
        ssize_t s;
        uint64_t u;

        s = read( event_fd_set[offset]  , &u, sizeof(uint64_t));
        if (s != sizeof(uint64_t))
                handle_error("read");

        int con_fd =  int( u );

        if (u == fd_used_init)
        {
            printf("first eventfd, skip it\n");
            continue;
        }

        if (debug)  printf("read offset:%d  data: (0x%lld)\n",    offset, (unsigned long long) u);

        //int fd = (int) u;
        //close(fd);
    }


}



struct sock_ev {
    struct event* read_ev;
    struct event* write_ev;
    char* buffer;
};

void release_sock_event(struct sock_ev* ev)
{
    event_del(ev->read_ev);
    free(ev->read_ev);
    free(ev->write_ev);
    free(ev->buffer);
    free(ev);
}

void on_write(int sock, short event, void* arg)
{
    char* buffer = (char*)arg;
    send(sock, buffer, strlen(buffer), 0);

    free(buffer);
}

void on_read(int sock, short event, void* arg)
{
    struct event* write_ev;
    int size;
    struct sock_ev* ev = (struct sock_ev*)arg;
    ev->buffer = (char*)malloc(MEM_SIZE);
    bzero(ev->buffer, MEM_SIZE);
    size = recv(sock, ev->buffer, MEM_SIZE, 0);
    printf("receive data:%s, size:%d\n", ev->buffer, size);

    //
    // 这里. read_ev 没有删除, 所以一直会读到size==0, 这里删除.
    // 所以 releaes_sock_event 没有删除 buffer 的操作.  
    // 真实的情况要复杂的多.
    //
    if (size == 0) {
        release_sock_event(ev);
        close(sock);
        return;
    }
    event_set(ev->write_ev, sock, EV_WRITE, on_write, ev->buffer);
    event_base_set(base, ev->write_ev);
    event_add(ev->write_ev, NULL);
}





void on_accept(int sock, short event, void* arg)
{
    struct sockaddr_in cli_addr;
    int newfd, sin_size;
    struct sock_ev* ev = (struct sock_ev*)malloc(sizeof(struct sock_ev));
    ev->read_ev = (struct event*)malloc(sizeof(struct event));
    ev->write_ev = (struct event*)malloc(sizeof(struct event));
    sin_size = sizeof(struct sockaddr_in);
    newfd = accept(sock, (struct sockaddr*)&cli_addr, (socklen_t *)&sin_size);


    // here write to work threads round-robin
    unsigned long long u;
    ssize_t s;
    u = (unsigned long long) newfd;

    s = write(event_fd_set[g_offset], &u, sizeof(uint64_t));
    if (s != sizeof(uint64_t))
          handle_error("write");
    if (debug) printf("write offset:%d  data: (%lld)\n",    g_offset, (unsigned long long) u);

    g_offset = (g_offset+1) % THREAD_NUMBER;
    /*
        event_set(ev->read_ev, newfd, EV_READ|EV_PERSIST, on_read, ev);
        event_base_set(base, ev->read_ev);
        event_add(ev->read_ev, NULL);
    */

}

int main(int argc, char* argv[])
{



    struct sockaddr_in my_addr;
    int sock;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(PORT);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&my_addr, sizeof(struct sockaddr));
    listen(sock, BACKLOG);








    //  工作线程相关的初始化

    fd_used_init = eventfd(0, 0);
    if ( fd_used_init == -1)
        handle_error("fd used init");

    for (unsigned int i=0; i<THREAD_NUMBER; i++)
    {
        thread_offset[i] = i; 

        event_fd_set[i] = eventfd(fd_used_init, 0);  
        printf("create event_fd %d\n",  event_fd_set[i] );
        if (event_fd_set[i] == -1)
        {
            handle_error("eventfd");
        }
    }
    printf("1111\n");


    pthread_t pt[THREAD_NUMBER]; 
    memset(pt, 0, sizeof(pt));
    for (unsigned int i=0; i< THREAD_NUMBER; i++)
    {
        if(pthread_create(&pt[i], NULL, child_main, (void*)(&thread_offset[i]) ) != 0)
        {
            perror("pthread_create error \n");
            return -1;
        }
    }
    printf("2222\n");


    // 接收请求的线程相关的初始化
    struct event listen_ev;
    base = event_base_new();
    event_set(&listen_ev, sock, EV_READ|EV_PERSIST, on_accept, NULL);
    event_base_set(base, &listen_ev);
    event_add(&listen_ev, NULL);
    event_base_dispatch(base);

    return 0;
}
