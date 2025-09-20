#
# Copyright (c) 2025 deadcafe.beef@gmail.com
#

CURDIR:=$(PWD)

CFLAGS  = -std=gnu11 -g -O3 -Werror -Wextra -Wall -Wvla -Wstrict-aliasing -pedantic -pipe
CPPFLAGS = -c -I$(CURDIR) -I../../include -D_GNU_SOURCE
LIBS =

#CFLAGS += -funroll-loops -frerun-loop-opt
#CFLAGS += -fforce-addr


SRCS    =       \
	test_rix_circleq.c

OBJS = ${SRCS:.c=.o}
DEPENDS = .depend
TARGET = circleq_test

.SUFFIXES:	.o .c
.PHONY:	all clean depend test
all:	depend $(TARGET)
test:
	./$(TARGET)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $<

$(TARGET):	$(OBJS)
	$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

$(OBJS):	Makefile

clean:
	rm -f $(OBJS) $(TARGET) $(DEPENDS) *~ core core.*

depend:	$(SRCS) Makefile
	-@ $(CC) $(CPPFLAGS) -MM -MG $(SRCS) > $(DEPENDS)

-include $(DEPENDS)
