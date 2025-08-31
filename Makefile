CC = gcc

CFLAGS = -Wall -Werror -std=c99 -pedantic

SRCS = quill.c

OUT = quill

all: $(OUT)

$(OUT): $(SRCS) 
	$(CC) $(CFLAGS) -o $(OUT) $(SRCS)

run: all 
	./$(OUT)

clean: rm -f $(OUT)
