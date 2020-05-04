# 编译

* make, 生成服务器server_process和客户端client_process, ip为本机默认ip, 端口9999

# 测试

* 方法一: 先 ./server_process & 把服务器抛后台, 再运行客户端 ./client_process

* 方法二: 直接运行测试脚本 ./start_test.sh &, 想提前停止测试则运行 ./kill_test.sh

* 方法三: 先 ./server_process & 把服务器抛后台, 再找个网页的在线websocket输入ip和端口测试