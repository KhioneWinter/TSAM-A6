//
// Simple chat server for TSAM
//
// Command line: ./server <port>
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
#include <list>
#include <poll.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include <ctime>


// // fix SOCK_NONBLOCK for OSX
// #ifndef SOCK_NONBLOCK
// #include <fcntl.h>
// #define SOCK_NONBLOCK O_NONBLOCK
// #endif

#define BACKLOG  5          // Allowed length of queue of waiting connections

// Framing: <SOH><length><STX><command><ETX>, length is 16 bits in network
// byte order and counts the whole frame including the 5 framing bytes.
#define SOH        0x01
#define STX        0x02
#define ETX        0x03
#define FRAME_OVERHEAD 5
#define MAX_FRAME  5000      // Longer frames are discarded

std::ofstream logFile;      // Timestamped log of everything the server does

// Current local time as "YYYY-MM-DD HH:MM:SS".
std::string timestamp()
{
    char buf[32];
    time_t now = time(NULL);
    struct tm tmNow;

    localtime_r(&now, &tmNow);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);

    return buf;
}

// Make arbitrary bytes safe to print: non-printable bytes are shown as \xNN.
std::string printable(const std::string &s)
{
    std::string out;
    char hex[8];

    for(unsigned char c : s)
    {
        if(c >= 0x20 && c < 0x7f)
        {
            out += c;
        }
        else
        {
            snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        }
    }
    return out;
}

// Write one timestamped line to server.log and to stdout.
void logMsg(const std::string &msg)
{
    std::string line = "[" + timestamp() + "] " + msg;

    std::cout << line << std::endl;

    if(logFile.is_open())
    {
        logFile << line << std::endl;   // endl flushes, so the log survives a crash
    }
}

//TODO: The server must listen on one TCP port so clients can connect to it
//TODO: The server must be able to maintain connections to multiple clients simultaniously,
            // it also has to react to commands from all of the clients without unreasonable delay
//TODO: The server must be able to handle a sequence of multiple requests per connection
//TODO: the server must operate auonomously!!
            // the server must not accept nor wait for any imput on the terminal.
            // All connection with the server must happen through TCP sockets
//TODO: you may run the server on TSAM server or on some other computer.
            // Note that in a later assignment you must run the server such that other groups can connect to it
//TODO: The executable of your server must be called "tsamserver", the server port you are listening on must be the fist command line argument
            // Example: ./tsamserver 4044
            // other command line arguments must be optional
//TODO: the server must keep a timestamp log of all commands sent and received,
            // this log may constain other information about the server state to facilitate debugging
//TODO: Do not hard code any IP adredded or port numbers, use additional command line arguments,
            // a config file or commands sent from you client if you need additional input




// Simple class for handling connections from clients.
//
// Client(int socket) - socket to send/receive traffic from client.
class Client
{
  public:
    int sock;              // socket of client connection
    std::string name;           // Limit length of name of client's user
    std::string inbuf;     // Bytes received but not yet part of a whole frame

    Client(int socket) : sock(socket){}

    ~Client(){}            // Virtual destructor defined for base class
};

std::map<int, Client*> clients; // Lookup table for per Client information

// Open socket for specified port.
//
// Returns -1 if unable to create the socket for any reason.

int open_socket(int portno)
{
   struct sockaddr_in sk_addr;   // address settings for bind()
   int sock;                     // socket opened for this port
   int set = 1;                  // for setsockopt

   // Create socket for connection. Set to be non-blocking, so recv will
   // return immediately if there isn't anything waiting to be read.
// #ifdef __APPLE__
//    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
//    {
//       perror("Failed to open socket");
//       return(-1);
//    }
// #else
   if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
   {
     perror("Failed to open socket");
    return(-1);
   }
// #endif

   // Turn on SO_REUSEADDR to allow socket to be quickly reused after
   // program exit.

   if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
   {
      perror("Failed to set SO_REUSEADDR:");
   }
   set = 1;
// #ifdef __APPLE__
//    if(setsockopt(sock, SOL_SOCKET, SOCK_NONBLOCK, &set, sizeof(set)) < 0)
//    {
//      perror("Failed to set SOCK_NOBBLOCK");
//    }
// #endif
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

// Wrap a command in <SOH><length><STX>...<ETX>.
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

// Pull the next complete frame out of buf and put its command in command.
// Garbage before a frame and malformed or too long frames are logged and
// thrown away. Returns false when buf holds no complete frame (yet).
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

      size_t len = ((unsigned char)buf[1] << 8) | (unsigned char)buf[2];

      if(len < FRAME_OVERHEAD || len > MAX_FRAME)
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

// Send a framed message to a client. send() may not be able to accept the
// whole message at once, so keep going until everything has been written.
//
// Returns false if the client has gone away.

bool sendMessage(int sock, const std::string &command)
{
   std::string msg  = frame(command);
   const char *p    = msg.c_str();
   size_t remaining = msg.length();

   logMsg("Sent to client " + std::to_string(sock) + ": " + printable(command));

   while(remaining > 0)
   {
      // MSG_NOSIGNAL: a client that has gone away must not kill the server
      int n = send(sock, p, remaining, MSG_DONTWAIT | MSG_NOSIGNAL);

      if(n < 0)
      {
         if(errno == EAGAIN || errno == EWOULDBLOCK)
         {
            continue;   // send buffer full, try again
         }
         return false;  // real error, client is gone
      }

      if(n == 0)        // client is gone
      {
         return false;
      }

      p         += n;
      remaining -= n;
   }

   return true;
}

// Process command from client on the server

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

int main(int argc, char* argv[])
{
    bool finished;
    int listenSock;                 // Socket for connections to server
    int clientSock;                 // Socket of connecting client

    // List of file descriptors being watched by poll(). By convention,
    // index 0 is always the listening socket; all other entries are
    // connected clients.
    std::vector<struct pollfd> fds;

    struct sockaddr_in client;
    socklen_t clientLen;
    char buffer[MAX_FRAME];         // buffer for reading from clients

    // Port is required, any further arguments are optional
    if(argc < 2)
    {
        printf("Usage: tsamserver <port>\n");
        exit(0);
    }

    // Open the log file, appending so earlier runs are kept

    logFile.open("server.log", std::ios::app);

    if(!logFile.is_open())
    {
        perror("Could not open server.log");
    }

    // Setup socket for server to listen to

    int port = atoi(argv[1]);
    listenSock = open_socket(port);

    if(listenSock < 0)
    {
        printf("Could not open socket on port %d\n", port);
        exit(1);
    }

    if(listen(listenSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %d\n", port);
        exit(1);
    }
    else
    // Add listen socket to the list of fds being polled.
    {
        fds.push_back({listenSock, POLLIN, 0});
        logMsg("Server listening on port " + std::to_string(port));
    }

    finished = false;

    while(!finished)
    {
        // Wait (indefinitely) until at least one of our sockets has
        // something to be read() on it.
        int n = poll(fds.data(), fds.size(), -1);

        if(n < 0)
        {
            perror("poll failed - closing down\n");
            finished = true;
        }
        else
        {
            // Take a snapshot of the current fds/revents before processing
            // them below, since fds itself may be modified while we go
            // (new clients connecting, existing clients disconnecting).
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
                   clients[clientSock] = new Client(clientSock);

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

                        inbuf.append(buffer, nread);

                        while(clients.count(sock) && extractFrame(sock, inbuf, command))
                        {
                            clientCommand(sock, &fds, command);
                        }
                    }
                }
            }
        }
    }
}
