# 编译

* make 生成执行程序 server 和 client, ip为本机默认ip, 端口9999

# 测试

* 方法一: 先 ./server & 把服务器抛后台, 再运行客户端 ./client

* 方法二: 直接运行测试脚本 ./start.sh &, 想提前停止测试则运行 ./kill.sh

* 方法三: 先 ./server & 把服务器抛后台, 再找个网页的在线websocket输入ip和端口测试

# 注意

* 1.在虚拟机里架服务器的话,最好用桥接的方式获得ip,net方式可能不通;

* 2.服务器示例代码未作过高并发压力测试,仅供开发参考;

* 3.关于服务端bind超时,通常是端口被占用或服务器关闭时有客户端未断开造成,后者会在1分钟后恢复正常.

# 其它

* websocket协议介绍: https://blog.csdn.net/SGuniver_22/article/details/74273839

* 欢迎提出bug、服务器和客户端优化建议、使用过程遇到的问题等等

* 有github帐号的可以左上角issues,或者上面csdn文章评论区提问.
