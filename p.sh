#!/usr/bin/expect -f
#set file [`*.h *.cpp]
spawn bash -c "scp *.cpp *.h build.sh  qiaoyong@192.168.1.107:~/qiaoyong/libevent"
expect  "*password:"
send "adidas2\n"
interact
