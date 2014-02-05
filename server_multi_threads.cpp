#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>

#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/eventfd.h>

#include <unistd.h>

#include "event.h"

#define PORT        25341
#define BACKLOG     5
#define MEM_SIZE    1024

int debug = 0;



struct event_base* base;
unsigned int g_offset = 0;



// 工作线程相关的配置文件
const unsigned int THREAD_NUMBER =  24;
struct event_base*  work_bases[ THREAD_NUMBER ];
int event_fd_set [THREAD_NUMBER];
unsigned int thread_offset [THREAD_NUMBER];

int fd_used_init = 0;  // 用来回避 eventfd 的 init value， 多一次 read 的问题


#define handle_error(msg)  do { perror(msg); exit(-1); } while (0)
//#define handle_error(msg)  do { perror(msg); exit(EXIT_FAILURE); } while (0)




struct sock_ev {
    struct event* read_ev;
    struct event* write_ev;
    char* read_buffer;

    int offset;
};




void release_sock_event(struct sock_ev* ev)
{
    if(ev->read_ev)  event_del(ev->read_ev);

    if(ev->read_ev)  event_free(ev->read_ev);
    if(ev->write_ev)  event_free(ev->write_ev);

    if(ev->read_buffer)  free(ev->read_buffer);
    ev->read_buffer = NULL;


    ev->offset = -1;

    if(ev)  free(ev);

}




void on_write(int sock, short event, void* arg)
{
    /* check 数据完整性， 再继续.   */

    usleep(1000);

    /*********************************
     *
     *    业务逻辑在这里, 不能 read。 否则一半的数据无法处理。
     *
     *********************************/

    struct sock_ev* ev = (struct sock_ev*) arg;

    // test: send ack + origin msg back
    send(sock, "ack:", 4, 0);
    send(sock, ev->read_buffer, strlen(ev->read_buffer), 0);

    free(ev->read_buffer);
    ev->read_buffer = NULL;
}


void on_read(int sock, short event, void* arg)
{
    int size;
    struct sock_ev* ev = (struct sock_ev*)arg;

    /* 真实的场景， 应该是只申请一次 */
    ev->read_buffer = (char*)malloc(MEM_SIZE);
    bzero(ev->read_buffer, MEM_SIZE);
    size = recv(sock, ev->read_buffer, MEM_SIZE, 0);
    
    if (size < 0)
    {
        // deal with "connection reset by peer", errno==104
        //printf("-------------ERROR:　%d, offset %d \n", errno, ev->offset);
        release_sock_event(ev);
        close(sock);
        return;        
    }

    //
    // 这里. read_ev 没有删除, 所以一直会读到size==0, 这里删除.
    // 所以 releaes_sock_event 没有删除 buffer 的操作.  
    // 真实的情况要复杂的多. a） 管道破裂； b） 慢速 socket 数据慢的问题
    // 对应的 read_buffer如何去弄， 这里都没有考虑
    // 
    if (size == 0) {
        if (errno != 0)
            printf("----------MISS: %d　offset %d, receive data:%s, size:%d\n", errno, ev->offset, ev->read_buffer, size);
        release_sock_event(ev);
        close(sock);
        return;
    }
    if(debug)  printf("offset %d receive data:%s, size:%d\n",ev->offset, ev->read_buffer, size);


    ev->write_ev = event_new( work_bases[ev->offset], sock,   EV_WRITE, on_write, (void*) ev );
    event_add(ev->write_ev, NULL);

}


// 从 eventfd 里面读取需要处理的 socket
void on_parse_socket(int sock, short event, void* arg)
{
    unsigned int * p = (unsigned int *)arg;
    unsigned int offset = *p; 


    ssize_t s;
    uint64_t u;

    s = read( sock  , &u, sizeof(uint64_t));
    if (s != sizeof(uint64_t))
            handle_error("read");
    int con_fd =  int( u );

    if (u == fd_used_init)
    {
        return;
    }

    if(debug) printf("read offset:%d  data: (0x%lld)\n",    offset, (unsigned long long) u);




    struct sock_ev* ev = (struct sock_ev*)malloc(sizeof(struct sock_ev));
    memset(ev, 0, sizeof(sock_ev) );
    ev->offset = offset;

    ev->read_ev = event_new( work_bases[offset], con_fd,   EV_READ|EV_PERSIST, on_read, (void*) ev );
    event_add(ev->read_ev, NULL);

}







void on_accept(int sock, short event, void* arg)
{
    struct sockaddr_in cli_addr;
    int newfd, sin_size;
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

    // simple round robin
    g_offset = (g_offset+1) % THREAD_NUMBER;

}



void * child_main(void* args)
{
    // child init
    unsigned int * p = (unsigned int *)args;
    unsigned int offset = *p; 


    work_bases[offset] = event_base_new();

    struct event * parse_sock_ev;
    parse_sock_ev = event_new( work_bases[offset], event_fd_set[offset], EV_READ|EV_PERSIST, on_parse_socket,  args   );
    event_add(parse_sock_ev, NULL);

    event_base_dispatch(work_bases[offset]);


    event_free(parse_sock_ev);

}






int main(int argc, char* argv[])
{
    signal(SIGPIPE,  SIG_IGN);


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


    // 接收请求的线程相关的初始化
    base = event_base_new();

    struct event * listen_ev;
    listen_ev = event_new( base, sock, EV_READ|EV_PERSIST, on_accept, NULL);
    event_add(listen_ev, NULL);


    event_base_dispatch(base);

    event_free(listen_ev);
    return 0;
}
