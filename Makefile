
#CC=arm-linux-gnueabihf-
CC=

target:
	$(CC)gcc -Wall -o client main_client.c ws_com.c -lpthread
	$(CC)gcc -Wall -o server main_server.c ws_com.c -lpthread

clean:
	@rm -rf client server
