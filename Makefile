.PHONY: format lintcheck clean cleansql

PLJS_FDW_VERSION = 1.1.0-dev

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR := ${shell $(PG_CONFIG) --includedir}
INCLUDEDIR_SERVER := ${shell $(PG_CONFIG) --includedir-server}

CP = cp
SRCS = fdw.c
OBJS = fdw.o
MODULE_big = pljs_fdw
EXTENSION = pljs_fdw
DATA = pljs_fdw.control pljs_fdw--$(PLJS_FDW_VERSION).sql
PG_CFLAGS += -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-declaration-after-statement \
    -Wno-cast-function-type -std=c11 -DPLJS_FDW_VERSION=\"$(PLJS_FDW_VERSION)\"
# Own copy of QuickJS, same as PLJS itself vendors -- see "PLJS function
# binding" in fdw.c for why this is not linked against pljs.so.
SHLIB_LINK = -Ldeps/quickjs -lquickjs

ifeq ($(DEBUG), 1)
PG_CFLAGS += -g
SHLIB_LINK += -g
endif

ifeq ($(DEBUG_MEMORY), 1)
PG_CFLAGS += -fno-omit-frame-pointer -fsanitize=address
SHLIB_LINK += -fsanitize=address
endif

all: deps/quickjs/quickjs.h deps/quickjs/libquickjs.a pljs_fdw--$(PLJS_FDW_VERSION).sql

include $(PGXS)

fdw.o: deps/quickjs/libquickjs.a

deps/quickjs/quickjs.h:
	mkdir -p deps
	git submodule update --init --recursive
	patch -p1 <patches/01-shared-lib-build
	patch -p1 <patches/02-unicode-conflict

deps/quickjs/libquickjs.a: deps/quickjs/quickjs.h
	cd deps/quickjs && make

format:
	clang-format -i $(SRCS)

lintcheck:
	clang-tidy $(SRCS) -- $(LINTFLAGS) -I$(INCLUDEDIR) -I$(INCLUDEDIR_SERVER) -I$(PWD) --std=c11

pljs_fdw--$(PLJS_FDW_VERSION).sql: pljs_fdw.sql
	$(CP) pljs_fdw.sql pljs_fdw--$(PLJS_FDW_VERSION).sql

clean: cleansql

cleansql:
	$(RM) -f pljs_fdw--$(PLJS_FDW_VERSION).sql
