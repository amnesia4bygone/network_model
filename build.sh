g++ -g server_multi_threads.cpp -I libevent/include/ -lpthread ./libevent/lib/libevent.a -lrt -o server
g++ -g server_single_thread.cpp -I libevent/include/ -lpthread ./libevent/lib/libevent.a -lrt -o server_single
