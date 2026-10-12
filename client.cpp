
// Command line: ./tsamclient <ip> <port>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netdb.h>
#include <string.h>
#include <vector>
#include <poll.h>
#include <time.h>
#include <string>

#include <iostream>



// Framing bytes format : <SOH> <length> <STX> <command> <ETX>
const char SOH = 0x01; // Marks the start of the frame
const char STX = 0x02; // marks the start of the text
const char ETX = 0x03; // marks the last byte of the frame


int connectingToServer(const char *portIp, const char *portPort) {
    //Connects to the server with the IP address and 
    // the port that was added in the command line 

    // hint is the address we want and the svr is the result
    struct addrinfo hints, *svr; 
    memset(&hints,   0, sizeof(hints)); // makes all the fields start at 0
    
    hints.ai_family   = AF_INET; // IPv4 only addresses
    hints.ai_socktype = SOCK_STREAM;

    // turns the IP and port text into a adress we can connect to
    int result = getaddrinfo(portIp, portPort, &hints, &svr); 
    // didnt get 0 so the result didnt work
    if (result != 0)
    {
        std::string errorText = gai_strerror(result); // get address info doesnt set errno
        std::cout << errorText << "\n";
        return -1;
    }

    // We create the TCP socket using the details from the result
    int sock = socket(svr->ai_family, svr->ai_socktype, svr->ai_protocol) ;
    if (sock < 0) {
        freeaddrinfo(svr); // free the addr result before we return the error
        perror("socket fail");
        return -1;
    }
    // connects to the server
    int connection = connect(sock, svr->ai_addr, svr->ai_addrlen);

    // if failed connection then we close the socket so avoid leaks
    if (connection < 0) {
        freeaddrinfo(svr);
        perror("connection fail to socket");
        close(sock);
        return -1;
    }

    freeaddrinfo(svr); // Result is no longer needed after the connection
    return sock;
}


std::string frameingMessage(const std::string &command) {
    int frameLength = command.length() +5 ; // command  plus the 5 bytes for the envalope
    std::string frame; // to build the frame

    frame += SOH; // start of the frame
    frame += (char)(frameLength/256); // High byte length, the amount of 256s
    frame += (char)(frameLength%256); // The low byte length, the remainder of the 256
    frame += STX; // starts of command text
    frame += command; // the command itself
    frame += ETX; // end of the frame

    return frame;
}

bool unwrapFrame(std::string &buffer, std::string &message) {
    size_t size;  
    while (true) {
        // we need the SOH + 2 bytes before the size can be read
        if (buffer.length() < 3) { 
            return false; 
        }

        // The frame needs to start with the SOH! 
        if (buffer[0] != SOH) {
            size_t possition = buffer.find(SOH);
            if (possition == std::string::npos) {
                // No SOH found
                buffer.clear();
                return false;
            }
            // erase everything until the SOH 
            buffer.erase(0, possition);
            continue;
        }
        // the size is the high bytes * 256 + the low bytes
        size = (unsigned char)buffer[1] * 256 + (unsigned char)buffer[2];
        // if the rest of the frame hasint arrived yet
        if (buffer.length() < size) {
            return false;
        }
        // the SOH is junk so we skip it (a real fram is 5 bytes at least)
        if (size < 5) {
            buffer.erase(0,1);
            continue;
        }

        // the ETX must be the end of the frame and the STX must follow the lwngth otherwise we skip the SOH
        if (buffer[3] != STX || buffer[size-1] != ETX) {
            buffer.erase(0,1);
            continue;
        }
        // the command is between the STX and ETX 
        message = buffer.substr(4, size-5);
        buffer.erase(0, size); // remove the whole frame

        return true; // a compleate frame was found!
    }

}


std::string timestamp() {
    // returns the current time in the ISO 8601 format!

    char timeArr[32]; // enough room for the characters
    time_t timeNow = time(nullptr); // now
    struct tm theTime; // holds the time split for year, month, day...
    gmtime_r(&timeNow, &theTime); // splits now into parts
    strftime(timeArr, sizeof(timeArr), "%Y-%m-%dT%H:%M:%S", &theTime); // writes the parts as text into the format

    return timeArr;
}


bool handlingServerData(int sock, std::string &buffer) {
    // Is called when the poll says the server socket is ready
    char temp[6000];// temporary space for one recv
    int n = recv(sock, temp, sizeof(temp), 0);

    // N is the number of bytes recieved so 0 would mean the server is closed
    // < 0 would be an error or a broken connection and > 0 would be valid!
    if (n == 0) {
        std::cout << "The server closed!\n" ;
        return false;
    }
    if (n < 0) {
        perror("error with server");
        return false;
    }
    // adds the new bytes into the buffer using N
    buffer.append(temp, n);

    // prints all complete frames
    std::string messageStr;
    while (unwrapFrame(buffer, messageStr)) {
        // showing what was recieved and when
        std::string currTimeStamp = timestamp();
        std::cout << "[" << currTimeStamp << "] RECEIVED: " << messageStr << "\n";
        
    }

    return true;
}



bool handlingUserData(int sock, std::string &inputBuffer) {
    // reads what the user types and sends the lines to the server
    char temp[6000]; // space for a reaf from the kayboard
    // N is the number of bytes types
    int n = read(STDIN_FILENO, temp, sizeof(temp));
    
    // when n is 0 then the user ended the input, 
    // when its less then 0 theres an error
    if (n == 0) {
        std::cout << "exiting\n" ;
        return false;
    }
    if (n < 0) {
        perror("error while reading input");
        return false;
    }

    // keeping the text until a whole line has arrived
    inputBuffer.append(temp, n);
    // checking for several lines
    while (true) {
        // finding the end of the line
        size_t possition = inputBuffer.find('\n');
        // there is no complete line so we wait for more
        if (possition == std::string::npos) {
            break;
        }
        // we extract the text from the line
        std::string line = inputBuffer.substr(0, possition);
        // remove it from the buffer
        inputBuffer.erase(0, possition +1);
        if (line.empty()) { // nothing to send
            continue;
        }
        // wrapping the command in a protocol frame
        std::string frame = frameingMessage(line);
        // sending the frame to the server
        if (send(sock, frame.data(), frame.length(), 0) < 0) {
            perror("the sending failed");
            return false;
        }
        // showing what was sent and when
        std::string currTimeStamp = timestamp();
        std::cout << "[" << currTimeStamp << "] SENT: " << line << "\n";
    }
    
    return true;
}




int main(int argc, char* argv[])
{
    // connects to the server and allows the user to input and recieve 
    int serverSocket; // the connection to the server 
    std::string serverBuffer; // the bytes from the server that are not yet a part of a complete frame
    std::string inputBuffer; // typed text not yet part of a complete line      
    bool finished; 

    // makes sure the user added the right set up for the command 
    if(argc < 3)
    {
        printf("Usage: tsamclient <ip>  <port>\n");
        printf("Ctrl-C to terminate\n");
        exit(1);
    }

    char *portIp = argv[1];
    char *portPort = argv[2];
    // we connect using the port and IP
    serverSocket = connectingToServer(portIp, portPort);

    if (serverSocket < 0) {
        exit(1);
    }

   // Watch both stdin (user typing a message) and the server socket
   // (server sending us something) with a single poll() call, instead of
   // using a separate thread for one of them.

    std::vector<struct pollfd> fds;
    fds.push_back({STDIN_FILENO,  POLLIN, 0}); //keybord
    fds.push_back({serverSocket,  POLLIN, 0}); // server

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

        // sleep until the server or the keybord has something
        // Something typed by the user - read a line and send it to the server.
        if(fds[0].revents & (POLLIN|POLLHUP|POLLERR))
        {
            if (!handlingUserData(serverSocket, inputBuffer)) {
                finished = true;
            }
        }

        // Something received from the server - print it out.
        if(fds[1].revents & (POLLIN|POLLHUP|POLLERR)) // prevents client busy loop if theres a server crash
        {
            if (!handlingServerData(serverSocket, serverBuffer)) {
                    finished = true;
                }
        }
    }
    close(serverSocket);
    return 0;
}

