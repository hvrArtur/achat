#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <thread>

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

using namespace std;

#define DEFAULT_BUFLEN 512
#define DEFAULT_PORT "27000"

SOCKET connect(string ip){
    int acres;
    SOCKET connectSocket = INVALID_SOCKET;
    struct addrinfo *result = NULL,
                    *ptr = NULL,
                    hints;

    ZeroMemory( &hints, sizeof(hints) );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    acres = getaddrinfo(&ip[0], DEFAULT_PORT, &hints, &result);
    if (acres != 0) {
        cout << "getaddrinfo failed with error: " << acres << endl;
        return 1;
    }

    for(ptr=result; ptr != NULL ;ptr=ptr->ai_next) {
        connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (connectSocket == INVALID_SOCKET) {
            cout << "socket failed with error: " << WSAGetLastError() << endl;
            return INVALID_SOCKET;
        }
        acres = connect(connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (acres == SOCKET_ERROR) {
            closesocket(connectSocket);
            connectSocket = INVALID_SOCKET;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (connectSocket == INVALID_SOCKET) {
        cout << "Unable to connect to server!" << endl;
        return INVALID_SOCKET;
    }
    return connectSocket;
}

void sendSocket(SOCKET socket){
    int acres;
    while(true){
        string buf;
        cin>>buf;
        cout<<"Sending: "<<buf<<endl;
        if(buf[0] == EOF) return;
        acres = send(socket, &buf[0], (int)buf.length(), 0);
        if (acres == SOCKET_ERROR) {
            cout << "send failed with error: " << WSAGetLastError() << endl;
            closesocket(socket);
            WSACleanup();
            return;
        }
    }
}

void listenSocket(SOCKET socket){
    int acres;
    char recvbuf[DEFAULT_BUFLEN];
    do {
        acres = recv(socket, recvbuf, DEFAULT_BUFLEN, 0);
        if (acres > 0)
            cout << recvbuf << endl;
        else if (acres == 0)
            cout << "Connection closed" << endl;
        else
            cout << "recv failed with error: "<< WSAGetLastError() << endl;
    } while (acres > 0);
}

int main(int argc, char **argv) 
{
    WSADATA wsaData;
    int acres;

    if (argc != 2) {
        cout << "usage: " << argv[0] << " server-name" << endl;
        return 1;
    }

    acres = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (acres != 0) {
        cout << "WSAStartup failed with error: " << acres << endl;
        return 1;
    }

    SOCKET socket = connect(argv[1]);
    if(socket == INVALID_SOCKET){
        cout << "Creation of listnening socket failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    thread t(listenSocket, socket);
    t.detach();
    thread t1(sendSocket, socket);
    t1.join();

    closesocket(socket);
    WSACleanup();

    return 0;
}