CFLAGS="-std=c++11 -D_GNU_SOURCE -fPIC -I../src -O3"
c++ $CFLAGS -c rxlib.cpp -o rxlib.cpp.o
cp ../build/librandomx.a .
