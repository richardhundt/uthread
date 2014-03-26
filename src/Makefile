COPT = -O2 -fPIC

CWARNS = -Wall

CFLAGS = $(CWARNS) $(COPT) -I../include

OS_NAME=$(shell uname -s)
MH_NAME=$(shell uname -m)

LDFLAGS=-ldl

ifeq ($(OS_NAME), Darwin)
LDFLAGS+=-bundle -undefined dynamic_lookup -framework CoreServices
ifeq ($(MH_NAME), x86_64)
endif
else
LDFLAGS+=-shared -lpthread
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

