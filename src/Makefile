COPT = -O2 -fPIC

CWARNS = -Wall

CFLAGS = $(CWARNS) $(COPT) -I../include

OS_NAME=$(shell uname -s)
MH_NAME=$(shell uname -m)

LDFLAGS=-ldl

ifeq ($(OS_NAME), Darwin)
LDFLAGS+=-bundle -undefined dynamic_lookup -framework CoreServices
else
ifneq (,$(findstring MINGW,$(OS_NAME)))
LDFLAGS+=-shared
COPT := -O2
else
LDFLAGS+=-shared -lpthread
endif
endif

SRCS := uthread.c
OBJS := $(patsubst %.c,%.o,$(SRCS))

all: $(OBJS) ./libuthread.so

./libuthread.so: $(OBJS)
	$(CC) $(COPT) uthread.c $(LIBS) -I../include -o ./libuthread.so $(LDFLAGS)

$(OBJS):
	$(CC) -c $(CFLAGS) $(SRCS)

clean:
	rm -f *.o *.so

.PHONY: all clean

