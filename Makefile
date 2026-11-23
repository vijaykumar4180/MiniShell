CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
TARGET = msh
OBJS = minishell.o job_control.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

minishell.o: minishell.c minishell.h
	$(CC) $(CFLAGS) -c minishell.c

job_control.o: job_control.c minishell.h
	$(CC) $(CFLAGS) -c job_control.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
