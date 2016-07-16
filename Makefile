# Fairly generic cross-compilation makefile for simple programs
CC=$(CROSSTOOL)/$(ARM)/bin/gcc
OBJS=pulse
LIBS=pthread

all: $(OBJS)
	$(CROSSTOOL)/$(ARM)/bin/strip $(OBJS)
	mv $(OBJS) $(OBJS).new

$(OBJS): $(OBJS).c
	$(CC) $(LDFLAGS) -o $@ $(OBJS).c -l$(LIBS)
