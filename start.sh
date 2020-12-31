
# 客户端数量
# 不要超过 main_server.c 中 CLIENT_MAX 定义的数量
cNum=100

echo ""
echo ">>>>>>>>>> 使用 ./kill.sh 关闭测试进程 <<<<<<<<<<"
echo ">>>>>>>>>>    测试在 25 秒后自动结束   <<<<<<<<<<"
echo ">>>>>>>>>> (避免混乱已关闭client的打印) <<<<<<<<<<"
echo ">>>>>>>>>>        客户端数量 $cNum      <<<<<<<<<<"
echo ""

sleep 3
./server &
sleep 1

while [ $cNum -gt 0 ] ; do
    cNum=`expr $cNum - 1`
    # 避免混乱,不看客户端打印
    ./client > /dev/null &
done

sleep 20

# 关闭进程
killall client
sleep 5
killall server

echo ""
echo ">>>>>>>>>> 测试结束 <<<<<<<<<<"
echo ""
