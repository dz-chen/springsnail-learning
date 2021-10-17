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
#include "conn.h"
#include "mgr.h"
#include "processpool.h"

using std::vector;

static const char* version = "1.0";

static void usage( const char* prog )
{   
    // 中括号表示参数非必须
    log( LOG_INFO, __FILE__, __LINE__,  "usage: %s [-h] [-v] [-f config_file]", prog );
}


/*
 * springnail程序的主函数
 * processpool是重点
 */ 
int main( int argc, char* argv[] )
{
    char cfg_file[1024];            // 配置文件名
    memset( cfg_file, '\0', 100 );
    int option;


    // 1.通过getopt获取命令行选项
    while ( ( option = getopt( argc, argv, "f:xvh" ) ) != -1 )
    {
        switch ( option )
        {
            case 'x':       // 设置日志级别
            {
                set_loglevel( LOG_DEBUG );
                break;
            }
            case 'v':       // 显示程序版本号
            {   
                log( LOG_INFO, __FILE__, __LINE__, "%s %s", argv[0], version );
                return 0;
            }
            case 'h':       // 显示提示
            {
                usage( basename( argv[ 0 ] ) );     // basename用于删除所有前缀
                return 0;
            }
            case 'f':       // 指定配置文件
            {
                memcpy( cfg_file, optarg, strlen( optarg ) );
                break;
            }
            case '?':
            {
                log( LOG_ERR, __FILE__, __LINE__, "un-recognized option %c", option );
                usage( basename( argv[ 0 ] ) );
                return 1;
            }
        }
    }    
    if( cfg_file[0] == '\0' )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "please specifiy the config file" );
        return 1;
    }


    // 2.读取配置文件
    int cfg_fd = open( cfg_file, O_RDONLY );
    if( !cfg_fd )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    struct stat ret_stat;
    if( fstat( cfg_fd, &ret_stat ) < 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    char* buf = new char [ret_stat.st_size + 1];    // 配置文件的所有内容(st_size字节)读取到buf中
    memset( buf, '\0', ret_stat.st_size + 1 );
    ssize_t read_sz = read( cfg_fd, buf, ret_stat.st_size );
    if ( read_sz < 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }


    // 3.解析配置文件
    vector< host > balance_srv;     // 负载均衡服务器(springnail本身) => 即xml顶部配置的服务器,它是springnail作为代理服务器向客户端提供的接口
    vector< host > logical_srv;     // 逻辑服务器                    => 即xml中配置的远程服务器
    host tmp_host;
    memset( tmp_host.m_hostname, '\0', 1024 );
    char* tmp_hostname;
    char* tmp_port;
    char* tmp_conncnt;
    bool opentag = false;       // 左标签(已找到)
    char* tmp = buf;
    char* tmp2 = NULL;
    char* tmp3 = NULL;
    char* tmp4 = NULL;
    while( tmp2 = strpbrk( tmp, "\n" ) )    
    {   
        // strpbrk: 依次检验字符串s1中的字符,当被检验字符在字符串s2中也包含时,则停止检验,并返回该字符内存位置
        *tmp2++ = '\0';
        if( strstr( tmp, "<logical_host>" ) )
        {   
            // strstr: 在字符串 haystack 中查找第一次出现字符串 needle 的位置,不包含终止符 '\0'
            if( opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            opentag = true;
        }
        else if( strstr( tmp, "</logical_host>" ) )
        {
            if( !opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            logical_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
            opentag = false;
        }
        else if( tmp3 = strstr( tmp, "<name>" ) )
        {
            tmp_hostname = tmp3 + 6;
            tmp4 = strstr( tmp_hostname, "</name>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            memcpy( tmp_host.m_hostname, tmp_hostname, strlen( tmp_hostname ) );
        }
        else if( tmp3 = strstr( tmp, "<port>" ) )
        {
            tmp_port = tmp3 + 6;
            tmp4 = strstr( tmp_port, "</port>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            tmp_host.m_port = atoi( tmp_port );
        }
        else if( tmp3 = strstr( tmp, "<conns>" ) )
        {
            tmp_conncnt = tmp3 + 7;
            tmp4 = strstr( tmp_conncnt, "</conns>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            tmp_host.m_conncnt = atoi( tmp_conncnt );
        }
        else if( tmp3 = strstr( tmp, "Listen" ) )
        {
            tmp_hostname = tmp3 + 6;
            tmp4 = strstr( tmp_hostname, ":" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4++ = '\0';
            tmp_host.m_port = atoi( tmp4 );
            memcpy( tmp_host.m_hostname, tmp3, strlen( tmp3 ) );
            balance_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
        }
        tmp = tmp2;
    }
    if( balance_srv.size() == 0 || logical_srv.size() == 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
        return 1;
    }


    // 4.启动负载均衡服务器(socket、bind、listent.....)
    const char* ip = balance_srv[0].m_hostname;
    int port = balance_srv[0].m_port;
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );   // a.创建监听socket,SOCK_STREAM代表TCP
    assert( listenfd >= 0 );
 
    int ret = 0;
    struct sockaddr_in address;                         // 服务端socket地址
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );   // b.将socket与地址绑定(即将socket地址与监听描述符绑定)
    assert( ret != -1 );

    ret = listen( listenfd, 5 );    // c.为监听socket创建监听队列,并进行监听
    assert( ret != -1 );


    // 5.创建进程池并投入运行......
    processpool< conn, host, mgr >* pool = processpool< conn, host, mgr >::create( listenfd, logical_srv.size() );
    if( pool )
    {
        pool->run( logical_srv );
        delete pool;
    }
    close( listenfd );
    return 0;
}
