# ===== 期中專案 Makefile（階段 0）=====
# 用法：
#   make            編譯全部（lib + server_naive + server_robust + client）
#   make DEBUG=1    額外加上 -DENABLE_DEBUG（階段 2 起 LOG_D 才會生效）
#   make clean      清除編譯產物

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -g -Iinclude
PORT    ?= 9000
CFLAGS  += -DDEFAULT_PORT=$(PORT)

ifeq ($(DEBUG),1)
CFLAGS  += -DENABLE_DEBUG
endif

# 為什麼用 rpath 而不是 LD_LIBRARY_PATH？
# $ORIGIN 代表「執行檔所在目錄」，所以 server/xxx 會到 server/../lib 找 .so，
# 不論從哪個目錄執行都找得到，換到另一台電腦也不用設環境變數。
# Makefile 裡 $ 要寫成 $$，單引號則是避免 shell 把 $ORIGIN 當成變數展開。
LDFLAGS := -Llib -Wl,-rpath,'$$ORIGIN/../lib'
LDLIBS  := -lutil

LIB     := lib/libutil.so
LIB_SRC := lib/util.c
HDR     := include/util.h

.PHONY: all lib clean server_naive server_robust client

all: lib server_naive server_robust client

lib: $(LIB)
server_naive: server/server_naive
server_robust: server/server_robust
client: client/client

# -fPIC：產生位置無關程式碼，.so 才能被載入到任意位址
# -shared：輸出共享函式庫而非執行檔
$(LIB): $(LIB_SRC) $(HDR)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

# 同一份 server.c 編兩個版本；差別只在 -DROBUST（CONTEXT.md B9）
server/server_naive: server/server.c $(HDR) $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

server/server_robust: server/server.c $(HDR) $(LIB)
	$(CC) $(CFLAGS) -DROBUST -o $@ $< $(LDFLAGS) $(LDLIBS)

client/client: client/client.c $(HDR) $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(LIB) server/server_naive server/server_robust client/client
