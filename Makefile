CC = gcc 

TARGET	=  $(patsubst %.c, %, $(wildcard *.c))
SRC 	=  $(wildcard *.c)
SRC 	+= $(wildcard *.h)


$(TARGET):$(SRC)
	$(CC) -o $@  ./src/sl_thread_pool.c ./src/sl_message_queue.c -I ./src/ $^ -lpthread -Wall -g

.PHONY: clean 
clean:
	rm -f $(TARGET)
