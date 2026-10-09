//
// Simple chat client for TSAM
//
// Command line: ./client <ip> <port>
//
// Author(s):
//    Jacky Mallett (jacky@ru.is)
//    Stephan Schiffel (stephans@ru.is)

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include <poll.h>

#include <iostream>
#include <sstream>


const char SOH = 0x01;
const char STX = 0x02;
const char ETX = 0x03;


//TODO: handling server data function, small stuff left
//TODO: the client must print out all commands sent and responses reseived with a human-readable timestamp (date and time)


int connectingToServer(const char *portIp, const char *portPort) {

    struct addrinfo hints, *svr;              // Network host entry for server
    memset(&hints,   0, sizeof(hints));
    
    hints.ai_family   = AF_INET;            // IPv4 only addresses
    hints.ai_socktype = SOCK_STREAM;

    int result = getaddrinfo(portIp, portPort, &hints, &svr);
    if (result != 0)
    // If we get 0 from the get address info then it worked
    {
        std::string errorText = gai_strerror(result);
        std::cout << errorText << "\n\n";
        return -1;
    }


    int sock = socket(svr->ai_family, svr->ai_socktype, svr->ai_protocol) ;
    if (sock < 0) {
        freeaddrinfo(svr);
        perror("socket fail");
        return -1;
    }
    int connection = connect(sock, svr->ai_addr, svr->ai_addrlen);

    if (connection < 0) {
        freeaddrinfo(svr);
        
        perror("connection fail to socket");
        close(sock);
        return -1;
    }

    freeaddrinfo(svr);

    return sock;
}


std::string frameingMessage(const std::string &command) {
    int frameLength = command.length() +5 ; // message size plus 5 bytes for the envalope

    std::string frame;
    frame += SOH; // adds one byte
    frame += (char)(frameLength/256); // length in high byte
    frame += (char)(frameLength%256); // length in low byte
    frame += STX; // adds one byte
    frame += command; // adds the message
    frame += ETX; // adds one byte

    return frame;

}

bool unwrapFrame(std::string &buffer, std::string &message) {
    if (buffer.length() < 3) {
        return false; 
    }
    size_t size = (unsigned char)buffer[1] * 256 + (unsigned char)buffer[2];
    
    if (buffer.length() < size) {
        return false;
    }
    
    size = size-5;
    message = buffer.substr(4, size);

    buffer.erase(0, size);

    return true;

}


bool handlingServerData(int sock, std::string &buffer) {
    // checks if the connection to the server is still active
    
    char temp[6000];
    // the amount of bytes that came in
    int n = recv(sock, temp, sizeof(temp), 0);


    if (n == 0) {
        std::cout << "The server closed!\n" ;
        return false;
    }
    if (n < 0) {
        perror("error with server");
        return false;
    }
    // stable connection
    buffer.append(temp, n);
    //TODO: unwrap the frames


    return true;
}

bool handlingUserData(int sock, std::string &inputBuffer) {
    // reads what the user types and sends the lines to the server
    
    char temp[6000];
    // the amount of bytes that came in
    // reads from keyboard
    int n = read(STDIN_FILENO, temp, sizeof(temp));


    if (n == 0) {
        std::cout << "exiting\n" ;
        return false;
    }
    if (n < 0) {
        perror("error while reading input");
        return false;
    }
    inputBuffer.append(temp, n);
    while (true) {
        size_t possition = inputBuffer.find('\n');
        if (possition == std::string::npos) {
            break;
        }
        std::string line = inputBuffer.substr(0, possition);
        inputBuffer.erase(0, possition +1);
        if (line.empty()) {
            continue;
        }
        std::string frame = frameingMessage(line);
        if (send(sock, frame.data(), frame.length(), 0), 0) {
            perror("the sending failed");
            return false;
        }
        
        std::cout << "sent: " << line << "\n";
    }
    
    return true;
}



int main(int argc, char* argv[])
{
    
    int serverSocket;                         // Socket used for server 
    std::string serverBuffer;
    std::string inputBuffer;                     
    bool finished;                   

    if(argc < 3)
    {
        printf("Usage: tsamclient <ip  port>\n");
        printf("Ctrl-C to terminate\n");
        exit(1);
    }

    char *portIp = argv[1];
    char *portPort = argv[2];
    serverSocket = connectingToServer(portIp, portPort);

    if (serverSocket < 0) {
        exit(1);
    }

   // Watch both stdin (user typing a message) and the server socket
   // (server sending us something) with a single poll() call, instead of
   // using a separate thread for one of them.

    std::vector<struct pollfd> fds;
    fds.push_back({STDIN_FILENO,  POLLIN, 0});
    fds.push_back({serverSocket,  POLLIN, 0});

    finished = false;
    while(!finished)
    {
        int n = poll(fds.data(), fds.size(), -1);

        if(n < 0)
        {
            perror("poll failed: ");
            finished = true;
            continue;
        }

        // Something typed by the user - read a line and send it to the server.
        if(fds[0].revents & (POLLIN|POLLHUP|POLLERR))// prevents client busy loop if theres a server crash
        {
            if (!handlingUserData(serverSocket, inputBuffer)) {
                finished = true;
            }
        }

        // Something received from the server - print it out.
        if(fds[1].revents & (POLLIN|POLLHUP|POLLERR)) 
        {
            if (!handlingServerData(serverSocket, serverBuffer)) {
                    finished = true;
                }
        }
    }
    close(serverSocket);
    return 0;
}

