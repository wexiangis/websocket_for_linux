
# 客户端数量
# 注意不要超过 main_server.c 中宏定义 CLIENT_MAX 定义的数量
cNum=10

echo ""
echo ">>>>>>>>>> 使用 kill_test.sh 关闭测试进程 <<<<<<<<<<"
echo ">>>>>>>>>>     测试在 20 秒后自动结束    <<<<<<<<<<"
echo ">>>>>>>>>>         客户端数量 $cNum       <<<<<<<<<<"

sleep 1
./server &
sleep 1

# 0.01秒间隔逐个接入客户端
while [ $cNum -gt 0 ] ; do
    cNum=`expr $cNum - 1`
    ./client &
    # sleep 0.01
done

sleep 20

# 关闭进程
killall client
sleep 5
killall server
sleep 1

echo ""
echo ">>>>>>>>>> 测试结束 <<<<<<<<<<"
echo ""
