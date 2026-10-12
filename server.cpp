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

#define BACKLOG  5          // How many connections may wait to be accepted

// Framing bytes: <SOH><length><STX><command><ETX>
#define SOH        0x01
#define STX        0x02
#define ETX        0x03
#define FRAME_OVERHEAD 5     // SOH + 2 length bytes + STX + ETX
#define MAX_FRAME  5000      // Longest command we accept, longer ones are thrown away


//  Logging

std::ofstream logFile;      // server.log, opened in main()

// Current local time as "YYYY-MM-DD HH:MM:SS".
std::string timestamp()
{
    char buf[32];
    time_t now = time(NULL);

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    return buf;
}

// Make bytes safe to print in the log: every non-printable byte becomes a '.'
std::string printable(std::string s)
{
    for(char &c : s)
    {
        if(c < 32 || c > 126)
        {
            c = '.';
        }
    }
    return s;
}

// Write one timestamped line to server.log and to the terminal.
void logMsg(const std::string &msg)
{
    std::string line = "[" + timestamp() + "] " + msg;

    std::cout << line << std::endl;
    logFile << line << std::endl;   // endl saves it right away, so the log survives a crash
}


//  Server state

// Information we keep about each connected client.
class Client
{
  public:
    std::string inbuf;     // Bytes received that are not a complete frame yet
};

// Connected clients, looked up by their socket.
std::map<int, Client*> clients;

// Stored messages for each username, oldest first. Each message is saved
// as "<from>,<message>". Messages belong to usernames, not connections,
// so they stay here after a client disconnects.
std::map<std::string, std::vector<std::string>> mailboxes;



//  Connections

// Create the socket the server listens on and bind it to the given port.
// Returns the socket, or -1 on failure.
int open_socket(int portno)
{
    struct sockaddr_in sk_addr;
    int sock;
    int set = 1;

    // Non-blocking, so accept() never hangs if a connection disappears
    // between poll() and accept()
    if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
    {
        perror("Failed to open socket");
        return(-1);
    }

    // Lets us restart the server on the same port right away
    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("Failed to set SO_REUSEADDR:");
    }

    // Listen on every network interface of this machine
    memset(&sk_addr, 0, sizeof(sk_addr));
    sk_addr.sin_family      = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port        = htons(portno);

    if(bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket:");
        return(-1);
    }

    return(sock);
}

// Close a client's connection and forget about it: stop polling its
// socket and free its entry in "clients". Its stored messages are kept.
void closeClient(int clientSocket, std::vector<struct pollfd> *fds)
{
    logMsg("Client " + std::to_string(clientSocket) + " disconnected");

    close(clientSocket);

    // Stop polling this socket
    auto fdIt = std::find_if(fds->begin(), fds->end(),
                    [clientSocket](const struct pollfd &pfd)
                    {
                        return pfd.fd == clientSocket;
                    });

    if(fdIt != fds->end())
    {
        fds->erase(fdIt);
    }

    // Free the client's information
    auto clientIt = clients.find(clientSocket);

    if(clientIt != clients.end())
    {
        delete clientIt->second;
        clients.erase(clientIt);
    }
}

//  Framing: <SOH><length><STX><command><ETX>

// Wrap a command in a frame. The length is 2 bytes in network byte order
// and counts the whole frame, including the 5 framing bytes.
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

// Take the next complete frame out of buf and put its command in "command".
// TCP can split or merge frames, so buf may hold half a frame or several.
// Junk before a frame and broken or too long frames are logged and thrown
// away. Returns false when there is no complete frame in buf yet.
bool extractFrame(int sock, std::string &buf, std::string &command)
{
    while(!buf.empty())
    {
        // Throw away anything before the start of a frame
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
            return false;          // length bytes have not arrived yet
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
            return false;          // rest of the frame has not arrived yet
        }

        if(buf[3] != STX || buf[len - 1] != ETX)
        {
            logMsg("Malformed frame from client " + std::to_string(sock)
                    + ", discarding: " + printable(buf.substr(0, len)));
            buf.erase(0, 1);       // drop this SOH and look for the next one
            continue;
        }

        // A complete frame: the command is between STX and ETX
        command = buf.substr(4, len - FRAME_OVERHEAD);
        buf.erase(0, len);
        return true;
    }
    return false;
}

// Frame a reply and send it to a client. send() may not send everything
// at once, so keep sending until the whole frame is out.
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
        int n = send(sock, p, remaining, MSG_NOSIGNAL);

        if(n < 0)
        {
            if(errno == EINTR)
            {
                continue;      // interrupted by a signal, try again
            }
            return false;      // real error, the client is gone
        }

        p         += n;
        remaining -= n;
    }

    return true;
}


//  Commands

// Split a command into the parts between the commas.
// "GETMSG,bob" becomes {"GETMSG", "bob"}.
std::vector<std::string> splitCommand(const std::string &command)
{
    std::vector<std::string> tokens;
    std::string token;
    std::stringstream stream(command);

    while(std::getline(stream, token, ','))
    {
        tokens.push_back(token);
    }

    return tokens;
}

// SENDMSG,<from>,<to>,<message>: store the message for <to>.
std::string handleSendMsg(const std::vector<std::string> &tokens)
{
    if(tokens.size() < 4)
    {
        return "ERROR,usage: SENDMSG,<from>,<to>,<message>";
    }

    std::string from = tokens[1];
    std::string to = tokens[2];

    if(from.empty() || to.empty())
    {
        return "ERROR,username is empty";
    }

    // The message may contain commas, so glue the rest of the parts back together
    std::string message = tokens[3];
    for(size_t i = 4; i < tokens.size(); i++)
    {
        message += "," + tokens[i];
    }

    mailboxes[to].push_back(from + "," + message);
    logMsg("Stored message from " + from + " for " + to);

    return "OK,message stored for " + to;
}

// GETMSG,<username>: return the oldest message for <username> and delete it.
std::string handleGetMsg(const std::vector<std::string> &tokens)
{
    if(tokens.size() != 2)
    {
        return "ERROR,usage: GETMSG,<username>";
    }

    std::string name = tokens[1];

    if(mailboxes.count(name) == 0)
    {
        return "NOMSG,no messages for " + name;
    }

    // Take the oldest message and remove it
    std::string message = mailboxes[name][0];
    mailboxes[name].erase(mailboxes[name].begin());

    // Remove the user when they have no messages left, so STATUSREQ skips them
    if(mailboxes[name].empty())
    {
        mailboxes.erase(name);
    }

    logMsg("Delivered message to " + name);

    return "MSG," + message;
}

// STATUSREQ: "<user1>:<count1>,<user2>:<count2>,..." or exactly "EMPTY".
std::string handleStatusReq()
{
    if(mailboxes.empty())
    {
        return "EMPTY";
    }

    std::string reply;

    for(auto &box : mailboxes)
    {
        reply += box.first + ":" + std::to_string(box.second.size()) + ",";
    }

    reply.pop_back();   // remove the last comma

    return reply;
}

// Handle one complete command from a client and send back the reply.
void clientCommand(int clientSocket, std::vector<struct pollfd> *fds,
    const std::string &command)
{
    std::string reply;

    logMsg("Received from client " + std::to_string(clientSocket) + ": " + printable(command));

    // The command name is everything up to the first comma
    std::string name = command.substr(0, command.find(','));

    if(name == "SENDMSG")
    {
        reply = handleSendMsg(splitCommand(command));
    }
    else if(name == "GETMSG")
    {
        reply = handleGetMsg(splitCommand(command));
    }
    else if(command == "STATUSREQ")
    {
        reply = handleStatusReq();
    }
    else
    {
        logMsg("Unknown command from client " + std::to_string(clientSocket)
                + ": " + printable(name));
        reply = "ERROR,unknown command";
    }

    if(!sendMessage(clientSocket, reply))
    {
        closeClient(clientSocket, fds);
    }
}

//  Main: set up the server, then wait for and handle events forever

int main(int argc, char* argv[])
{
    bool finished;
    int listenSock;                 // Socket new clients connect to
    int clientSock;                 // Socket of a newly connected client

    // Sockets watched by poll(). fds[0] is always the listening socket,
    // the rest are connected clients.
    std::vector<struct pollfd> fds;

    struct sockaddr_in client;      // Address of a newly connected client
    socklen_t clientLen;
    char buffer[MAX_FRAME];         // Bytes from one recv()

    // ---- Setup ----------------------------------------------------------

    if(argc < 2)
    {
        printf("Usage: tsamserver <port>\n");
        exit(0);
    }

    // Append to the log, so earlier runs are kept
    logFile.open("server.log", std::ios::app);

    if(!logFile.is_open())
    {
        perror("Could not open server.log");
    }

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

    fds.push_back({listenSock, POLLIN, 0});
    logMsg("Server listening on port " + std::to_string(port));

    // ---- Main loop ------------------------------------------------------

    finished = false;

    while(!finished)
    {
        // Sleep until a socket has something for us (no busy-waiting)
        int n = poll(fds.data(), fds.size(), -1);

        if(n < 0)
        {
            // A signal woke us up, nothing is wrong, just wait again
            if(errno == EINTR)
            {
                continue;
            }

            perror("poll failed - closing down");
            finished = true;
            continue;
        }

        // Work on a copy, because fds changes when clients connect or
        // disconnect while we go through the list
        std::vector<struct pollfd> readyFds = fds;

        // ---- New connections --------------------------------------------

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
                fds.push_back({clientSock, POLLIN, 0});
                clients[clientSock] = new Client();

                logMsg("Client " + std::to_string(clientSock) + " connected from "
                        + inet_ntoa(client.sin_addr) + ":"
                        + std::to_string(ntohs(client.sin_port)));
            }
        }

        // ---- Data from clients ------------------------------------------

        for(size_t i = 1; i < readyFds.size(); i++)
        {
            if(readyFds[i].revents & (POLLIN | POLLHUP | POLLERR))
            {
                int sock = readyFds[i].fd;

                // 0 means the client closed the connection,
                // below 0 means an error
                int nread = recv(sock, buffer, sizeof(buffer), MSG_DONTWAIT);

                if(nread < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    continue;      // not a real error, try again on the next poll()
                }

                if(nread <= 0)
                {
                    closeClient(sock, &fds);
                }
                else
                {
                    // Add the new bytes to this client's buffer and handle
                    // every complete frame in it. Stop if a failed reply
                    // closed the client.
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
