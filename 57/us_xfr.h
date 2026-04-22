/* us_xfr.h

   Header file for us_xfr_sv.c and us_xfr_cl.c.
*/
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SV_SOCK_PATH "/tmp/us_xfr"  //通信地址，一个文件路径
#define BUF_SIZE 100                 //缓冲区大小，一次能传递的最大字节数