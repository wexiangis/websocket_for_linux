#!/usr/bin/python3
# -*- coding:utf-8 -*-

# 安装: pip3 install websocket-server
# 参考: https://blog.csdn.net/weixin_37989267/article/details/87928934
from websocket_server import WebsocketServer
 
def new_client(client, server):
    print('new_client', client['id'])
    server.send_message_to_all('Say hi~ I am server')

def client_left(client, server):
    print('client_left', client['id'])

# 自动回复ping包的逻辑有待添加...
def message_received(client, server, message):
    print('message_received', client['id'], message)
    server.send_message_to_all(message)

if __name__ == '__main__':

    server = WebsocketServer(9999, "127.0.0.1")
    server.set_fn_new_client(new_client)
    server.set_fn_client_left(client_left)
    server.set_fn_message_received(message_received)
    server.run_forever()
