#include <stdio.h>
#include "util.h"

/* 階段 0：client 只印出函式庫版本，證明能成功連結並載入 libutil.so。 */
int main(void)
{
    printf("client using %s\n", util_version());
    return 0;
}
