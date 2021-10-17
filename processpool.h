#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>
#include "log.h"
#include "fdwrapper.h"

using std::vector;


/*
 * process类代表进程
 */
class process
{
public:
    process() : m_pid( -1 ){}

public:
    int m_busy_ratio;       // 描述此进程的繁忙程度
    pid_t m_pid;            // 进程id
    int m_pipefd[2];        // 通过socketpair()创建的本地套接字(其实就是一个双向管道); 创建时父进程关闭写端,子进程关闭读端,然后用于父子进程通信
};


/*
 * processpool是进程池的模板类
 * 模板参数C: conn 
 * 模板参数H: host
 * 模板参数M: mgr
 * 1.要理解进程池的原理,注意在子进程的虚拟地址空间中也是存在此进程池的实例的,由于COW写时才会复制;
 * 2.子进程中通过进程池的m_idx来判断当前是哪个进程的;
 */
template< typename C, typename H, typename M >
class processpool
{
private:
    processpool( int listenfd, int process_number = 8 );
public:

    /*
     * static函数,创建进程池实例,返回指向此实例的指针
     * 会调用processpool的构造函数
     */
    static processpool< C, H, M >* create( int listenfd, int process_number = 8 )
    {
        if( !m_instance )
        {
            m_instance = new processpool< C, H, M >( listenfd, process_number );
        }
        return m_instance;
    }
    ~processpool()
    {
        delete [] m_sub_process;
    }

    void run( const vector<H>& arg );

private:
    void notify_parent_busy_ratio( int pipefd, M* manager );
    int get_most_free_srv();
    void setup_sig_pipe();
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16;
    static const int USER_PER_PROCESS = 65536;
    static const int MAX_EVENT_NUMBER = 10000;
    int m_process_number;                           // 进程数量
    int m_idx;                                      // 当前进程的下标,注意在子进程中这个变量被修改为其对应的下标,后续的逻辑需要通过它判断是哪个进程
    int m_epollfd;                                  // 存放epoll感兴趣事件的描述符
    int m_listenfd;                                 // 监听描述符
    int m_stop;
    process* m_sub_process;                         // 指向进程池中的子进程(数组)
    static processpool< C, H, M >* m_instance;      // 静态变量,指向进程池实例的指针;默认为NULL
};



template< typename C, typename H, typename M >
processpool< C, H, M >* processpool< C, H, M >::m_instance = NULL;

static int EPOLL_WAIT_TIME = 5000;
static int sig_pipefd[2];           // setup_sig_pipe()中被初始化


/*
 * 信号处理函数,它作为handler处理产生的信号
 * 此项目中,它其实就是将信号信息写入管道
 */
static void sig_handler( int sig )
{
    int save_errno = errno;         // TODO:暂存errno的作用
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}


/*
 * 注册信号处理函数;
 * 注意:信号不同于中断,信号处理函数在用户态执行,而中断处理在内核态执行
 */
static void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


/*
 * 进程池构造函数
 * 作用:创建需要的子进程;初始化进程池的监听描述符
 */ 
template< typename C, typename H, typename M >
processpool< C, H, M >::processpool( int listenfd, int process_number ) 
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( ( process_number > 0 ) && ( process_number <= MAX_PROCESS_NUMBER ) );

    m_sub_process = new process[ process_number ];
    assert( m_sub_process );

    for( int i = 0; i < process_number; ++i )
    {
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd );
        assert( ret == 0 );

        m_sub_process[i].m_pid = fork();                // 注意:子进程中也有一份 m_sub_process[i].m_pid,且被修改
        assert( m_sub_process[i].m_pid >= 0 );
        if( m_sub_process[i].m_pid > 0 )                // 1.父进程(返回子进程id)
        {
            close( m_sub_process[i].m_pipefd[1] );
            m_sub_process[i].m_busy_ratio = 0;
            continue;   // 父进程中,继续创建下一个子进程
        }   
        else                                            // 2.子进程(返回0)
        {
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i;  // 很关键,在子进程中这个变量被修改,后续的逻辑需要通过它判断是哪个进程!
            break;      // 子进程中,则直接结束执行
        }
    }
}


/*
 * 找出进程池中最不繁忙的进程,返回其下标
 */ 
template< typename C, typename H, typename M >
int processpool< C, H, M >::get_most_free_srv()
{
    int ratio = m_sub_process[0].m_busy_ratio;
    int idx = 0;
    for( int i = 0; i < m_process_number; ++i )
    {
        if( m_sub_process[i].m_busy_ratio < ratio )
        {
            idx = i;
            ratio = m_sub_process[i].m_busy_ratio;
        }
    }
    return idx;
}


/*
 * TODO:
 */
template< typename C, typename H, typename M >
void processpool< C, H, M >::setup_sig_pipe()
{
    m_epollfd = epoll_create( 5 );
    assert( m_epollfd != -1 );

    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] );
    add_read_fd( m_epollfd, sig_pipefd[0] );

    // TODO
    addsig( SIGCHLD, sig_handler );     // 当子进程终止时,会向父进程发送一个SIGCHLD
    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    addsig( SIGPIPE, SIG_IGN );
}


/**
 * 
 */ 
template< typename C, typename H, typename M >
void processpool< C, H, M >::run( const vector<H>& arg )
{
    if( m_idx != -1 )           // 如果当前控制流是子进程
    {
        run_child( arg );
        return;
    }
    run_parent();
}



/**
 * TODO:
 */ 
template< typename C, typename H, typename M >
void processpool< C, H, M >::notify_parent_busy_ratio( int pipefd, M* manager )
{
    int msg = manager->get_used_conn_cnt();
    send( pipefd, ( char* )&msg, 1, 0 );    
}




template< typename C, typename H, typename M >
void processpool< C, H, M >::run_child( const vector<H>& arg )
{
    setup_sig_pipe();

    int pipefd_read = m_sub_process[m_idx].m_pipefd[ 1 ];
    add_read_fd( m_epollfd, pipefd_read );

    epoll_event events[ MAX_EVENT_NUMBER ];

    M* manager = new M( m_epollfd, arg[m_idx] );
    assert( manager );

    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        if( number == 0 )
        {
            manager->recycle_conns();
            continue;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( ( sockfd == pipefd_read ) && ( events[i].events & EPOLLIN ) )
            {
                int client = 0;
                ret = recv( sockfd, ( char* )&client, sizeof( client ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 ) 
                {
                    continue;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof( client_address );
                    int connfd = accept( m_listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        log( LOG_ERR, __FILE__, __LINE__, "errno: %s", strerror( errno ) );
                        continue;
                    }
                    add_read_fd( m_epollfd, connfd );
                    C* conn = manager->pick_conn( connfd );
                    if( !conn )
                    {
                        closefd( m_epollfd, connfd );
                        continue;
                    }
                    conn->init_clt( connfd, client_address );
                    notify_parent_busy_ratio( pipefd_read, manager );
                }
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )
            {
                 RET_CODE result = manager->process( sockfd, READ );
                 switch( result )
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else if( events[i].events & EPOLLOUT )
            {
                 RET_CODE result = manager->process( sockfd, WRITE );
                 switch( result )
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else
            {
                continue;
            }
        }
    }

    close( pipefd_read );
    close( m_epollfd );
}




template< typename C, typename H, typename M >
void processpool< C, H, M >::run_parent()
{
    setup_sig_pipe();

    for( int i = 0; i < m_process_number; ++i )
    {
        add_read_fd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }

    add_read_fd( m_epollfd, m_listenfd );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == m_listenfd )
            {
                /* 这部分是原作者注释的...
                int i =  sub_process_counter;
                do
                {
                    if( m_sub_process[i].m_pid != -1 )
                    {
                        break;
                    }
                    i = (i+1)%m_process_number;
                }
                while( i != sub_process_counter );
                
                if( m_sub_process[i].m_pid == -1 )
                {
                    m_stop = true;
                    break;
                }
                sub_process_counter = (i+1)%m_process_number;
                */
                int idx = get_most_free_srv();
                send( m_sub_process[idx].m_pipefd[0], ( char* )&new_conn, sizeof( new_conn ), 0 );
                log( LOG_INFO, __FILE__, __LINE__, "send request to child %d", idx );
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    for( int i = 0; i < m_process_number; ++i )
                                    {
                                        if( m_sub_process[i].m_pid == pid )
                                        {
                                            log( LOG_INFO, __FILE__, __LINE__, "child %d join", i );
                                            close( m_sub_process[i].m_pipefd[0] );
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                m_stop = true;
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    if( m_sub_process[i].m_pid != -1 )
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                log( LOG_INFO, __FILE__, __LINE__, "%s", "kill all the clild now" );
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if( pid != -1 )
                                    {
                                        kill( pid, SIGTERM );
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )
            {
                int busy_ratio = 0;
                ret = recv( sockfd, ( char* )&busy_ratio, sizeof( busy_ratio ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                for( int i = 0; i < m_process_number; ++i )
                {
                    if( sockfd == m_sub_process[i].m_pipefd[0] )
                    {
                        m_sub_process[i].m_busy_ratio = busy_ratio;
                        break;
                    }
                }
                continue;
            }
        }
    }

    for( int i = 0; i < m_process_number; ++i )
    {
        closefd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }
    close( m_epollfd );
}

#endif
