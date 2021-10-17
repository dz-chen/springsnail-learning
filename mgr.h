#ifndef SRVMGR_H
#define SRVMGR_H

#include <map>
#include <arpa/inet.h>
#include "fdwrapper.h"
#include "conn.h"

using std::map;

/*
 *      host类代表一台主机,仅有成员变量
 * 成员m_hostname: 主机名
 * 成员m_port:     端口号
 * 成员m_conncnt:  支持的最大连接数
 */
class host
{
public:
    char m_hostname[1024];
    int m_port;
    int m_conncnt;
};


class mgr
{
public:
    mgr( int epollfd, const host& srv );
    ~mgr();
    int conn2srv( const sockaddr_in& address );
    conn* pick_conn( int sockfd );
    void free_conn( conn* connection );
    int get_used_conn_cnt();
    void recycle_conns();
    RET_CODE process( int fd, OP_TYPE type );

private:
    static int m_epollfd;
    map< int, conn* > m_conns;
    map< int, conn* > m_used;
    map< int, conn* > m_freed;
    host m_logic_srv;
};

#endif
