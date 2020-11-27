
echo ""
echo ">>>>>>>>>> 使用 kill_test.sh 关闭测试进程 <<<<<<<<<<"
echo ">>>>>>>>>>     测试在 30 秒后自动结束    <<<<<<<<<<"
echo ""

sleep 1
./server &
sleep 1

# 客户端数量
clientNum=50

# 0.5秒间隔逐个接入
while [ $clientNum -gt 0 ] ; do
    clientNum=`expr $clientNum - 1`
    ./client &
    # sleep 0.5
done

sleep 30

# 关闭进程
killall client
sleep 1
killall server
sleep 0.5

echo ""
echo ">>>>>>>>>> 测试结束 <<<<<<<<<<"
echo ""
