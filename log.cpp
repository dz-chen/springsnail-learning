#include <stdio.h>
#include <time.h>
#include <string.h>
#include "log.h"

static int level = LOG_INFO;
static int LOG_BUFFER_SIZE = 2048;
static const char* loglevels[] =
{
    "emerge!", "alert!", "critical!", "error!", "warn!", "notice:", "info:", "debug:"
};

void set_loglevel( int log_level )
{
    level = log_level;
}

/*
 *      日志框架
 * log_level:按哪个级别打印日志
 * file_name:在哪个文件中打印的日志
 * line_num: 答应此日志的行
 * format:格式化打印
 * 
 * 示例:log( LOG_ERR, __FILE__, __LINE__, "%s", "the client read buffer is full, let server write" );
 * 其中宏 __FILE__ 表示当前文件; 宏 __LINE__ 表示当前行,他们都是C的标准宏,预处理时由编译器替换
 */ 
void log( int log_level,  const char* file_name, int line_num, const char* format, ... )
{
    if ( log_level > level )
    {
        return;
    }

    time_t tmp = time( NULL );
    struct tm* cur_time = localtime( &tmp );
    if ( ! cur_time )
    {
        return;
    }

    char arg_buffer[ LOG_BUFFER_SIZE ];
    memset( arg_buffer, '\0', LOG_BUFFER_SIZE );
    strftime( arg_buffer, LOG_BUFFER_SIZE - 1, "[ %x %X ] ", cur_time );
    printf( "%s", arg_buffer );
    printf( "%s:%04d ", file_name, line_num );
    printf( "%s ", loglevels[ log_level - LOG_EMERG ] );

    va_list arg_list;                                   // 承接可变参数构成的format的参数列表
    va_start( arg_list, format );
    memset( arg_buffer, '\0', LOG_BUFFER_SIZE );
    vsnprintf( arg_buffer, LOG_BUFFER_SIZE - 1, format, arg_list );
    printf( "%s\n", arg_buffer );
    fflush( stdout );
    va_end( arg_list );
}
