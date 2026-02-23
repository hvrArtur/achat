cmake -G "MinGW Makefiles" . -B build && cmake --build build --target buildAll
||
cmake -G "MinGW Makefiles" . -B build
if ($?) { cmake --build build --target buildAll }

./build/client/achatclient.exe 147.32.123.58 {ip of server}
./build/server/achatserver.exe