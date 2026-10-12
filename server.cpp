//
// Simple mail server for TSAM assignment A6.
//
// Command line: ./tsamserver <port>
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
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include <poll.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>

#define BACKLOG  5          // Allowed length of queue of waiting connections

#define SOH        0x01
#define STX        0x02
#define ETX        0x03
#define FRAME_OVERHEAD 5
#define MAX_FRAME 5000

std::ofstream logFile;

std::string timestamp()
{
    char buf[32];
    time_t now = time(nullptr);
    struct tm tmNow;

#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    return buf;
}

std::string printable(const std::string &s)
{
    std::string out;
    char hex[8];

    for (unsigned char c : s)
    {
        if (c >= 0x20 && c < 0x7f)
        {
            out += static_cast<char>(c);
        }
        else
        {
            snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        }
    }
    return out;
}

void logMsg(const std::string &msg)
{
    const std::string line = "[" + timestamp() + "] " + msg;
    std::cout << line << std::endl;
    if (logFile.is_open())
    {
        logFile << line << std::endl;
    }
}


// Simple class for handling connections from clients.
// Holds the per-client state, keyed by socket in "clients".
class Client
{
  public:
    std::string inbuf;     // Bytes received but not yet part of a whole frame
};

std::map<int, Client*> clients; // Lookup table for per Client information

int open_socket(int portno)
{
    struct sockaddr_in sk_addr;
    int sock;
    int set = 1;

   // Create socket for connection. Set to be non-blocking, so recv will
   // return immediately if there isn't anything waiting to be read.

   if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
   {
     perror("Failed to open socket");
    return(-1);
   }

   // Turn on SO_REUSEADDR to allow socket to be quickly reused after
   // program exit.

   if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
   {
      perror("Failed to set SO_REUSEADDR:");
   }
    memset(&sk_addr, 0, sizeof(sk_addr));

    sk_addr.sin_family      = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port        = htons(portno);

   // Bind to socket to listen for connections from clients

    if(bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket:");
        return(-1);
    }
    else
    {
        return(sock);
    }
}

// Close a client's connection, remove it from the list of file
// descriptors being watched by poll(), and free its entry in the
// "clients" map.

void closeClient(int clientSocket, std::vector<struct pollfd> *fds)
{
    logMsg("Client " + std::to_string(clientSocket) + " disconnected");

    close(clientSocket);

    // Find and remove this socket's entry from the list of fds polled.
    auto fdIt = std::find_if(fds->begin(), fds->end(),
                    [clientSocket](const struct pollfd &pfd)
                    {
                        return pfd.fd == clientSocket;
                    });

    if(fdIt != fds->end())
    {
        fds->erase(fdIt);
    }

    // Remove the client's record and free it.
    auto clientIt = clients.find(clientSocket);

    if(clientIt != clients.end())
    {
        delete clientIt->second;
        clients.erase(clientIt);
    }
}

std::string frame(const std::string &command)
{
    uint16_t len = htons(command.length() + FRAME_OVERHEAD);
    std::string out;

    out += (char)SOH;
    out.append((const char *)&len, 2);
    out += (char)STX;
    out += command;
    out += (char)ETX;

    return out;
}

bool extractFrame(int sock, std::string &buf, std::string &command)
{
    while(!buf.empty())
    {
        // Skip anything before the start of a frame
        size_t start = buf.find((char)SOH);

        if(start == std::string::npos)
        {
            logMsg("Discarded " + std::to_string(buf.size()) + " bytes of garbage from client "
                    + std::to_string(sock) + ": " + printable(buf));
            buf.clear();
            return false;
        }
        if(start > 0)
        {
            logMsg("Discarded " + std::to_string(start) + " bytes of garbage from client "
                + std::to_string(sock) + ": " + printable(buf.substr(0, start)));
            buf.erase(0, start);
        }

        if(buf.size() < 3)
        {
            return false;          // length field not here yet
        }

        size_t len = (((unsigned char)buf[1] << 8) | (unsigned char)buf[2]);

        if( len < FRAME_OVERHEAD || len > MAX_FRAME + FRAME_OVERHEAD)
        {
            logMsg("Invalid frame length " + std::to_string(len) + " from client "
                + std::to_string(sock) + ", discarding");
            buf.erase(0, 1);       // drop this SOH and look for the next one
            continue;
        }

        if(buf.size() < len)
        {
            return false;          // rest of the frame not here yet
        }

        if(buf[3] != STX || buf[len - 1] != ETX)
        {
            logMsg("Malformed frame from client " + std::to_string(sock)
                    + ", discarding: " + printable(buf.substr(0, len)));
            buf.erase(0, 1);
            continue;
        }

        command = buf.substr(4, len - FRAME_OVERHEAD);
        buf.erase(0, len);
        return true;
    }
    return false;
}

bool sendMessage(int sock, const std::string &command)
{
    std::string msg  = frame(command);
    const char *p    = msg.c_str();
    size_t remaining = msg.length();

    logMsg("Sent to client " + std::to_string(sock) + ": " + printable(command));

    while(remaining > 0)
    {
        // MSG_NOSIGNAL: a client that has gone away must not kill the server
        int n = send(sock, p, remaining, MSG_NOSIGNAL);

        if(n < 0)
        {
            if(errno == EINTR )
            {
            continue;   // interrupted by a signal,  try again
            }
            return false;  // real error, client is gone
        }

        p         += n;
        remaining -= n;
    }

    return true;
}

bool parseGetMessage(const std::string &command, std::string &username)
{
    if (command.compare(0, 7, "GETMSG,") != 0)
    {
        return false;
    }

    username = command.substr(7);
    return !username.empty();
}

void clientCommand(int clientSocket, std::vector<struct pollfd> *fds,
    const std::string &command)
{
    std::vector<std::string> tokens;
    std::string token;

    logMsg("Received from client " + std::to_string(clientSocket) + ": " + printable(command));

    // Split command from client into comma separated tokens for parsing
    std::stringstream stream(command);

    while(std::getline(stream, token, ','))
        tokens.push_back(token);

    if(tokens.empty())
        return;

    // For now just acknowledge every command so the client sees a reply
    if(!sendMessage(clientSocket, "Received: " + tokens[0]))
    {
        closeClient(clientSocket, fds);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: ./tsamserver <port>\n");
        return 0;
    }

    logFile.open("server.log", std::ios::app);
    if (!logFile.is_open())
    {
        perror("Could not open server.log");
    }

    const int port = atoi(argv[1]);
    const int listenSock = open_socket(port);
    if (listenSock < 0)
    {
        printf("Could not open socket on port %d\n", port);
        return 1;
    }

    if (listen(listenSock, BACKLOG) < 0)
    {
        perror("Listen failed");
        close(listenSock);
        return 1;
    }

    std::vector<struct pollfd> fds;
    fds.push_back({listenSock, POLLIN, 0});
    logMsg("Server listening on port " + std::to_string(port));

    char buffer[MAX_FRAME];
    struct sockaddr_in client;
    socklen_t clientLen = sizeof(client);

    bool finished = false;
    while (!finished)
    {
        int n = poll(fds.data(), fds.size(), -1);
        if (n < 0)
        {
            perror("poll failed");
            finished = true;
            continue;
        }

        std::vector<struct pollfd> readyFds = fds;

            // First, accept any new connections to the server on the listening socket
            if(readyFds[0].revents & POLLIN)
            {
                clientLen = sizeof(client);
                clientSock = accept(listenSock, (struct sockaddr *)&client,
                                    &clientLen);

                if(clientSock < 0)
                {
                    perror("accept failed");
                }
                else
                {
                    // Add new client to the list of fds being polled
                    fds.push_back({clientSock, POLLIN, 0});

                   // create a new client to store information.
                    clients[clientSock] = new Client();

                    logMsg("Client " + std::to_string(clientSock) + " connected from "
                            + inet_ntoa(client.sin_addr) + ":"
                            + std::to_string(ntohs(client.sin_port)));
                }
            }
            // Now check for commands from clients
            for(size_t i = 1; i < readyFds.size(); i++)
            {
                if(readyFds[i].revents & (POLLIN | POLLHUP | POLLERR))
                {
                    int sock = readyFds[i].fd;

                    // recv() == 0 means client has closed connection,
                    // recv() < 0 means the connection broke (e.g. reset).
                    int nread = recv(sock, buffer, sizeof(buffer), MSG_DONTWAIT);

                    if(nread <= 0)
                    {
                        closeClient(sock, &fds);
                    }
                    else
                    {
                        // TCP is a byte stream: add what arrived to this
                        // client's buffer and handle every complete frame.
                        std::string &inbuf = clients[sock]->inbuf;
                        std::string command;

            while (extractFrame(sock, inbuf, command))
            {
                clientCommand(sock, &fds, command);
            }
        }
    }

    close(listenSock);
    return 0;
}