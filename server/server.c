#include <stdio.h>
#include "util.h"

/*
 * 階段 0：server 只印出函式庫版本，證明能成功連結並在執行期載入 libutil.so。
 * 同一份原始碼會編成 server_naive 與 server_robust（差別在 -DROBUST），
 * 這裡先印出目前是哪個版本，確認 Makefile 的巨集有傳進來。
 */
int main(void)
{
#ifdef ROBUST
    const char *variant = "robust";
#else
    const char *variant = "naive";
#endif
    printf("server (%s) using %s\n", variant, util_version());
    return 0;
}
