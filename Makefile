#CFLAGS  += -std=c99 -Wall -O2 -D_REENTRANT
CFLAGS  += -std=c99 -Wall -g -D_REENTRANT

LIBS    := -lpthread -lm -lssl -lcrypto

TARGET  := $(shell uname -s | tr '[A-Z]' '[a-z]' 2>/dev/null || echo unknown)

ifeq ($(TARGET), sunos)
	CFLAGS += -D_PTHREADS -D_POSIX_C_SOURCE=200112L
	LIBS   += -lsocket
else ifeq ($(TARGET), darwin)
	LDFLAGS += -pagezero_size 10000 -image_base 100000000
else ifeq ($(TARGET), linux)
	CFLAGS  += -D_POSIX_C_SOURCE=200112L -D_BSD_SOURCE
	LIBS    += -ldl
	LDFLAGS += -Wl,-E
else ifeq ($(TARGET), freebsd)
	CFLAGS  += -D_DECLARE_C99_LDBL_MATH
	LDFLAGS += -Wl,-E
endif

SRC  := wrk.c net.c ssl.c aprintf.c stats.c script.c units.c \
        ae.c zmalloc.c http_parser.c hdr_histogram.c tinymt64.c \

BIN  := wrk3
VER  ?= $(shell git describe --tags --always --dirty)

OBJDIR := obj
OBJ  := $(patsubst %.c,$(OBJDIR)/%.o,$(SRC)) $(OBJDIR)/bytecode.o $(OBJDIR)/version.o
LIBS := -lluajit-5.1 $(LIBS)

DEPS    :=
CFLAGS  += -I$(OBJDIR)/include -I$(OBJDIR)/include/luajit-2.0
LDFLAGS += -L$(OBJDIR)/lib

OPENSSL_URL=https://www.openssl.org/source/openssl-1.1.0g.tar.gz
LUAJIT_URL=https://luajit.org/download/LuaJIT-2.0.5.tar.gz

ifneq ($(WITH_LUAJIT),)
	CFLAGS  += -I$(WITH_LUAJIT)/include
	LDFLAGS += -L$(WITH_LUAJIT)/lib
else
	DEPS += $(OBJDIR)/lib/libluajit-5.1.a
endif

ifneq ($(WITH_OPENSSL),)
	CFLAGS  += -I$(WITH_OPENSSL)/include
	LDFLAGS += -L$(WITH_OPENSSL)/lib
else
	DEPS += $(OBJDIR)/lib/libssl.a
endif

all: $(BIN)

clean:
	$(RM) -rf $(BIN) obj/*

deps:
	rm -rf deps
	mkdir -p deps
	wget -P $(PWD)/deps $(OPENSSL_URL)
	wget -P $(PWD)/deps $(LUAJIT_URL)

$(BIN): $(OBJ)
	@echo LINK $(BIN)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJ): config.h Makefile $(DEPS) | $(OBJDIR)

$(OBJDIR):
	@mkdir -p $@

$(OBJDIR)/bytecode.o: src/wrk.lua
	@echo LUAJIT $<
	@$(SHELL) -c 'PATH=obj/bin:$(PATH) luajit -b $(CURDIR)/$< $(CURDIR)/$@'

$(OBJDIR)/version.o:
	@echo 'const char *VERSION="$(VER)";' | $(CC) -xc -c -o $@ -

$(OBJDIR)/%.o : %.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

###########################################################################
#  Dependencies build section
###########################################################################
LUAJIT  := $(notdir $(patsubst %.tar.gz,%,$(wildcard deps/LuaJIT*.tar.gz)))
OPENSSL := $(notdir $(patsubst %.tar.gz,%,$(wildcard deps/openssl*.tar.gz)))

OPENSSL_OPTS = no-shared no-psk no-srp no-dtls no-idea --prefix=$(abspath $(OBJDIR))

$(OBJDIR)/$(LUAJIT):  deps/$(LUAJIT).tar.gz  | $(OBJDIR)
	@tar -C $(OBJDIR) -xf $<

$(OBJDIR)/$(OPENSSL): deps/$(OPENSSL).tar.gz | $(OBJDIR)
	@tar -C $(OBJDIR) -xf $<

$(OBJDIR)/lib/libluajit-5.1.a: $(OBJDIR)/$(LUAJIT)
	@echo Building LuaJIT...
	@$(MAKE) -C $< PREFIX=$(abspath $(OBJDIR)) BUILDMODE=static install

$(OBJDIR)/lib/libssl.a: $(OBJDIR)/$(OPENSSL)
	@echo Building OpenSSL...
ifeq ($(TARGET), darwin)
	@$(SHELL) -c "cd $< && ./Configure $(OPENSSL_OPTS) darwin64-x86_64-cc"
else
	@$(SHELL) -c "cd $< && ./config $(OPENSSL_OPTS)"
endif
	@$(MAKE) -C $< depend
	@$(MAKE) -C $<
	@$(MAKE) -C $< install_sw
	@touch $@


.DEFAULT_GOAL := all

.PHONY: all clean
.PHONY: $(OBJDIR)/version.o

.SUFFIXES:
.SUFFIXES: .c .o .lua

vpath %.c   src
vpath %.h   src
vpath %.lua scripts
