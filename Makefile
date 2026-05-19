all:
	gcc -Wall -pthread -o server server.c threadpool.c

clean:
	rm -f server