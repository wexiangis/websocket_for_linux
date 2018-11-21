
echo ""
echo ">>>>>>>>>> 使用 kill_test.sh 关闭测试进程 <<<<<<<<<<"
echo ""

sleep 1
./server_process &
sleep 1

clientNum=5

while [ $clientNum -gt 0 ] ; do

    clientNum=`expr $clientNum - 1`

    ./client_process &

done

sleep 30
./kill_test.sh

echo ""
echo ">>>>>>>>>> 测试结束 <<<<<<<<<<"
echo ""