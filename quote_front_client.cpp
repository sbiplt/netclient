#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <memory.h>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#define PRINT_BUFF_COUNT 100 
#define BUF_SIZE 8000

#define printf(...) 

int log_fd = -1;
int max_fds = 1;
char svr_ip[32] = "192.168.1.229";

boost::lockfree::spsc_queue<int, 
        boost::lockfree::capacity<1024> , 
        boost::lockfree::fixed_sized<true> > read_queue;

boost::lockfree::spsc_queue<int, 
        boost::lockfree::capacity<1024> , 
        boost::lockfree::fixed_sized<true> > del_queue;

boost::lockfree::spsc_queue<char*, 
        boost::lockfree::capacity<PRINT_BUFF_COUNT> , 
        boost::lockfree::fixed_sized<true> > print_buff;

boost::lockfree::spsc_queue<char*, 
        boost::lockfree::capacity<PRINT_BUFF_COUNT> , 
        boost::lockfree::fixed_sized<true> > print_data;


bool setnonblocking(int socket_fd) {
    int opt;

    opt = fcntl(socket_fd, F_GETFL);
    if (opt < 0) {
        printf("fcntl(F_GETFL) fail.");
        return -1;
    }
    opt |= O_NONBLOCK;
    if (fcntl(socket_fd, F_SETFL, opt) < 0) {
        printf("fcntl(F_SETFL) fail.");
        return -1;
    }
    return 0;
}

void handle_sig(int signum)
{
    printf("receiv sig int");
    sleep(5);
    exit(0);
}

int get_socket()
{
    int connect_fd;
    int ret;
    const int port = 11700;

    static struct sockaddr_in srv_addr;
    connect_fd = socket(PF_INET, SOCK_STREAM, 0);
    if(connect_fd < 0)
    {
        perror("cannot create communication socket");
        return -1;
    }

    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(svr_ip);
    srv_addr.sin_port = htons(port);

    ret = connect(connect_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if(ret == -1)
    {
        perror("cannot connect to the server");
        close(connect_fd);
        return -1;
    }

    if (setnonblocking(connect_fd) < 0)
    {
        return -1;
    }

    if(log_fd == -1){
        log_fd = connect_fd;
    }
    return connect_fd;
}

void print_handler()
{
    boost::thread th(
    []{
        char *buf;
        while(true){
            while(print_data.pop(buf))
            {
                if(*buf)
                {
                    std::cout << "recv: " << buf << std::endl;;
                }
                print_buff.push(buf);
            }
            boost::this_thread::sleep(boost::posix_time::milliseconds(5));
        }
    });
    th.detach();
}

void read_handler()
{
    boost::thread th(
    []{
        int fd;
        char buf[BUF_SIZE + 1];
        while(true){
            while(read_queue.pop(fd))
            {
                int len = 0;
                if(fd != log_fd)
                {
                    do
                    {
                        len = read(fd, buf, BUF_SIZE);
                    }while(len > 0);
                }
                else
                {
                    char *pbuff;
                    if(!print_buff.pop(pbuff))
                    {
                        pbuff = buf;
                        std::cout << "print buffer exhaust " << std::endl;
                    }
                    len = read(fd, pbuff, BUF_SIZE);

                    while( len > 0 )
                    {
                        if(pbuff != buf)
                        {
                            pbuff[len] = '\0'; 
                            print_data.push(pbuff);
                        }

                        if(!print_buff.pop(pbuff))
                        {
                            pbuff = buf;
                            std::cout << "print buffer exhaust " << std::endl;
                        }
                        len = read(fd, pbuff, BUF_SIZE);
                    }

                    if(pbuff != buf)
                    {
                        pbuff[len] = '\0'; 
                        print_data.push(pbuff);
                    }
                }

                if (len == 0) {
                    std::cout << "recv zero data, close:" << fd << std::endl;
                    del_queue.push(fd);
                }
                else if (len == -1 && errno != EAGAIN) {
                    std::cout << "errno: " << errno << "  close fd:" << fd << std::endl;
                    del_queue.push(fd);
                } 
            }
            boost::this_thread::sleep(boost::posix_time::milliseconds(5));
        }
    });
    th.detach();
}

void epoll_loop(int max_fds)
{
    int ep_fd = epoll_create(max_fds);

    struct epoll_event ev, evs[max_fds];
    for(int i = 0; i < max_fds; i++)
    {
        int fd = get_socket();
        if(fd > 0)
        {
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
            epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &ev);
        }
    }

    int ev_fd;
    while (true) {
        int nfds = epoll_wait(ep_fd, evs, max_fds, -1);
        for (int i = 0; i < nfds; i++) {
            if (evs[i].events & EPOLLIN) {
                if ((ev_fd = evs[i].data.fd) > 0) {
                    printf("epollin event fd:%d\n", ev_fd);
                    read_queue.push(ev_fd);
                }
            } else if(evs[i].events & EPOLLOUT){
                if ((ev_fd = evs[i].data.fd) > 0) {
                    printf("receive epoll out fd:%d\n", ev_fd);
                    char out_data[] = "{(len=29)MARKET01@@S@+@@@&NASD,AAPL.US}";
                    write(ev_fd, out_data, sizeof(out_data) - 1);

                    ev.events = EPOLLIN | EPOLLET;
                    epoll_ctl(ep_fd, EPOLL_CTL_ADD, ev_fd, &ev);
                }
            }
        }

        int del_fd;
        while(del_queue.pop(del_fd))
        {
            epoll_ctl(ep_fd, EPOLL_CTL_DEL, del_fd, &ev);
            close(del_fd);
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sig);
    
    if(argc > 1)
    {
        max_fds = std::stoi(argv[1]);
    }
    std::cout << "max_fds: " << max_fds << std::endl;

    if(argc > 2)
    {
        strncpy(svr_ip, argv[2], sizeof(svr_ip) - 1);
    }
    std::cout << "svr_ip: " << svr_ip << std::endl;

    read_handler();
    for(int i = 0; i < PRINT_BUFF_COUNT; i++)
    {
        print_buff.push(new char[BUF_SIZE]);
    }
    print_handler();
    epoll_loop(max_fds); 
    
    return 0;
}

