g++ server_multi_threads.cpp -I libevent/include/ -lpthread ./libevent/lib/libevent.a -lrt -o server
g++ server_single_thread.cpp -I libevent/include/ -lpthread ./libevent/lib/libevent.a -lrt -o server_single
