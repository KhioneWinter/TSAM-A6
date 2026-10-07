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
#include <sstream>
#include <thread>
#include <map>


// // fix SOCK_NONBLOCK for OSX
// #ifndef SOCK_NONBLOCK
// #include <fcntl.h>
// #define SOCK_NONBLOCK O_NONBLOCK
// #endif

#define BACKLOG  5          // Allowed length of queue of waiting connections

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

// Close a client's connection, and remove it from the list of file
// descriptors being watched by poll().
//
// Note: this only removes the socket from the poll() watch list. It does
// NOT remove the corresponding entry from the "clients" map - callers are
// responsible for that (see the main loop below).

void closeClient(int clientSocket, std::vector<struct pollfd> *fds)
{
     printf("Client closed connection: %d\n", clientSocket);

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
}

// Send a message to a client. send() may not be able to accept the whole
// message at once, so keep going until everything has been written.
//
// Returns false if the client has gone away.

bool sendMessage(int sock, const std::string &msg)
{
   const char *p    = msg.c_str();
   size_t remaining = msg.length();

   while(remaining > 0)
   {
      int n = send(sock, p, remaining, MSG_DONTWAIT);

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
                  char *buffer) 
{
  std::vector<std::string> tokens;
  std::string token;

  // Split command from client into tokens for parsing
  std::stringstream stream(buffer);

  while(stream >> token)
      tokens.push_back(token);

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
    char buffer[1025];              // buffer for reading from clients

    if(argc != 2)
    {
        printf("Usage: chat_server <ip port>\n");
        exit(0);
    }

    // Setup socket for server to listen to

    listenSock = open_socket(atoi(argv[1])); 
    printf("Listening on port: %d\n", atoi(argv[1]));

    if(listen(listenSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", argv[1]);
        exit(0);
    }
    else 
    // Add listen socket to the list of fds being polled.
    {
        fds.push_back({listenSock, POLLIN, 0});
    }

    finished = false;

    while(!finished)
    {
        memset(buffer, 0, sizeof(buffer));

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
               clientSock = accept(listenSock, (struct sockaddr *)&client,
                                   &clientLen);
            //    printf("accept***\n");
               // Add new client to the list of fds being polled
               fds.push_back({clientSock, POLLIN, 0});

               // create a new client to store information.
               // Re-use the record for this socket if we have seen it
               // before, so we don't leak a Client every time someone
               // disconnects and reconnects.
            //    if(clients.find(clientSock) == clients.end())
            //    {
            clients[clientSock] = new Client(clientSock);
            //    }

               // Decrement the number of sockets waiting to be dealt with
               n--;

               printf("Client connected on server: %d\n", clientSock);
            }
            // Now check for commands from clients
            std::list<Client *> disconnectedClients;  
            for(size_t i = 1; i < readyFds.size(); i++)
            {
                if(readyFds[i].revents & POLLIN)
                {
                    int sock = readyFds[i].fd;
                    Client *client = clients[sock];

                    // recv() == 0 means client has closed connection
                    if(recv(sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                    {
                        disconnectedClients.push_back(client);
                        closeClient(sock, &fds);

                    }
                    // We don't check for -1 (nothing received) because poll()
                    // only triggers if there is something on the socket for us.
                    else
                    {
                        // std::cout << buffer << std::endl;
                        clientCommand(sock, &fds, buffer);
                    }
                }
               // Remove client from the clients list
               for(auto const& c : disconnectedClients)
                  clients.erase(c->sock);
            }
        }
    }
}
