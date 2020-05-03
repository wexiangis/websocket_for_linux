
#CC=arm-linux-gnueabihf-
CC=

target:
	$(CC)gcc -Wall -O3 -o client_process client_main.c websocket_common.c -lpthread
	$(CC)gcc -Wall -O3 -o server_process server_main.c websocket_common.c -lpthread

clean:
	@rm -rf client_process server_process
