#include "util.h"

/* 階段 0：佔位用的實作，只為了讓 libutil.so 有一個可被引用的符號。 */
const char *util_version(void)
{
    return "libutil 0.0 (stage 0)";
}
