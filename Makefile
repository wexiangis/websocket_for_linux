
#CC=arm-linux-gnueabihf-
CC=

target:
	$(CC)gcc -Wall -o client ./c_test/main_client.c ./c_test/ws_com.c ./c_test/ws_server.c -I./c_test -lpthread
	$(CC)gcc -Wall -o server ./c_test/main_server.c ./c_test/ws_com.c ./c_test/ws_server.c -I./c_test -lpthread

clean:
	@rm -rf client server
