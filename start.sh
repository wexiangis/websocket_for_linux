
# 客户端数量
# 不要超过 main_server.c 中 CLIENT_MAX 定义的数量(1000)
# 普通计算机接入破千之后CPU会逐渐拉满,无法再接入/发起更多客户端(性能因素)
cNum=10

echo ""
echo ">>>>>>>>>> 使用 ./kill.sh 关闭测试进程 <<<<<<<<<<"
echo ">>>>>>>>>>    测试在 25 秒后自动结束   <<<<<<<<<<"
echo ">>>>>>>>>> (避免混乱已关闭client的打印) <<<<<<<<<<"
echo ">>>>>>>>>>        客户端数量 $cNum       <<<<<<<<<<"

sleep 3
./server &
sleep 1

# 0.01秒间隔逐个接入客户端
while [ $cNum -gt 0 ] ; do
    cNum=`expr $cNum - 1`
    # 避免混乱,不看客户端程序打印
    ./client > /dev/null &
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
