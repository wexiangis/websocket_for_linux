# 编译

* make 生成执行程序 server 和 client, ip为本机默认ip, 端口9999

# 测试

* 方法一: 先 ./server & 把服务器抛后台, 再运行客户端 ./client

* 方法二: 直接运行测试脚本 ./test_start.sh &, 想提前停止测试则运行 ./test_kill.sh

* 方法三: 先 ./server & 把服务器抛后台, 再找个网页的在线websocket输入ip和端口测试

* 在虚拟机里架服务器的话, 最好用桥接的方式获得IP