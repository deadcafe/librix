#
# Copyright (c) 2025 deadcafe.beef@gmail.com
#

CURDIR:=$(PWD)

CFLAGS  = -std=gnu11 -g -O3 -Werror -Wextra -Wall -Wvla -Wstrict-aliasing -pedantic -pipe
CPPFLAGS = -c -I$(CURDIR) -I../../include -D_GNU_SOURCE
DEPFLAGS = -MMD -MP
LIBS =

#CFLAGS += -funroll-loops -frerun-loop-opt
#CFLAGS += -fforce-addr


SRCS    =       \
	test_rix_stailq.c

OBJS = ${SRCS:.c=.o}
DEPS = ${OBJS:.o=.d}
TARGET = stailq_test

.SUFFIXES:	.o .c
.PHONY:	all clean test
all:	$(TARGET)
test:
	./$(TARGET)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ $<

$(TARGET):	$(OBJS)
	$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

$(OBJS):	Makefile

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) *~ core core.*

-include $(DEPS)
