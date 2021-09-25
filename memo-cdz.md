@[toc]
# 使用
1.当前目录make编译项目  
2.修改config.xml,顶部是springnail作为代理服务器提供给客户端的ip和端口;xml是代理服务器最终要访问的远程服务器  
3.执行 ./springnail -f config.xml  
4.执行nc ip:port  
5.执行nc后,输入http请求,从而自动通过springnail作为代理,访问config.xml中配置的服务器  


# 各文件作用
main.cpp:主函数  
log.h、log.cpp:日志程序  
conn.h、conn.cpp:客户端类  
fdwrapper.h、fdwrapper.cpp:文件描述符fd相关的函数  
mgr.h、mgr.cpp:处理网络连接和负载均衡的框架  
processpool.h:进程池  
config.xml:指明服务器本身的地址及要连接的IP  

