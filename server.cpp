//
// Simple mail server for TSAM assignment A6.
//
// Usage: ./tsamserver <port>

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
#include <queue>
#include <poll.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <set>

#define BACKLOG  5
#define SOH      0x01
#define STX      0x02
#define ETX      0x03
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

class Client
{
  public:
    int sock;
    std::string name;
    std::string inbuf;

    explicit Client(int socket) : sock(socket) {}
};

std::map<int, Client *> clients;
std::map<std::string, std::queue<std::string> > mailboxes;

int open_socket(int portno)
{
    struct sockaddr_in sk_addr;
    int sock;
    int set = 1;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Failed to open socket");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("Failed to set SO_REUSEADDR");
    }

    memset(&sk_addr, 0, sizeof(sk_addr));
    sk_addr.sin_family = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port = htons(static_cast<uint16_t>(portno));

    if (bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket");
        close(sock);
        return -1;
    }

    return sock;
}

void closeClient(int clientSocket, std::vector<struct pollfd> *fds)
{
    logMsg("Client " + std::to_string(clientSocket) + " disconnected");

    auto fdIt = std::find_if(fds->begin(), fds->end(),
                             [clientSocket](const struct pollfd &pfd) {
                                 return pfd.fd == clientSocket;
                             });
    if (fdIt != fds->end())
    {
        fds->erase(fdIt);
    }

    auto clientIt = clients.find(clientSocket);
    if (clientIt != clients.end())
    {
        delete clientIt->second;
        clients.erase(clientIt);
    }

    close(clientSocket);
}

std::string frame(const std::string &command)
{
    uint16_t len = htons(static_cast<uint16_t>(command.length() + FRAME_OVERHEAD));
    std::string out;
    out += static_cast<char>(SOH);
    out.append(reinterpret_cast<const char *>(&len), 2);
    out += static_cast<char>(STX);
    out += command;
    out += static_cast<char>(ETX);
    return out;
}

bool extractFrame(int sock, std::string &buf, std::string &command)
{
    while (!buf.empty())
    {
        size_t start = buf.find(static_cast<char>(SOH));
        if (start == std::string::npos)
        {
            logMsg("Discarded " + std::to_string(buf.size()) + " bytes of garbage from client " +
                   std::to_string(sock) + ": " + printable(buf));
            buf.clear();
            return false;
        }

        if (start > 0)
        {
            logMsg("Discarded " + std::to_string(start) + " bytes of garbage from client " +
                   std::to_string(sock) + ": " + printable(buf.substr(0, start)));
            buf.erase(0, start);
        }

        if (buf.size() < 3)
        {
            return false;
        }

        size_t len = (static_cast<unsigned char>(buf[1]) << 8) |
                     static_cast<unsigned char>(buf[2]);

        if (len < FRAME_OVERHEAD || len > MAX_FRAME)
        {
            logMsg("Invalid frame length " + std::to_string(len) + " from client " +
                   std::to_string(sock) + ", discarding");
            buf.erase(0, 1);
            continue;
        }

        if (buf.size() < len)
        {
            return false;
        }

        if (buf[3] != STX || buf[len - 1] != ETX)
        {
            logMsg("Malformed frame from client " + std::to_string(sock) +
                   ", discarding: " + printable(buf.substr(0, len)));
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
    std::string msg = frame(command);
    const char *p = msg.c_str();
    size_t remaining = msg.length();

    logMsg("Sent to client " + std::to_string(sock) + ": " + printable(command));

    while (remaining > 0)
    {
        int n = send(sock, p, remaining, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

std::string mailboxStatus()
{
    if (mailboxes.empty())
    {
        return "EMPTY";
    }

    std::vector<std::string> names;
    names.reserve(mailboxes.size());
    for (const auto &entry : mailboxes)
    {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());

    std::ostringstream output;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0)
        {
            output << ",";
        }
        output << names[i] << ":" << mailboxes[names[i]].size();
    }
    return output.str();
}

bool parseSendMessage(const std::string &command, std::string &from, std::string &to, std::string &message)
{
    if (command.compare(0, 8, "SENDMSG,") != 0)
    {
        return false;
    }

    std::string rest = command.substr(8);
    const size_t first = rest.find(',');
    if (first == std::string::npos)
    {
        return false;
    }
    const size_t second = rest.find(',', first + 1);
    if (second == std::string::npos)
    {
        return false;
    }

    from = rest.substr(0, first);
    to = rest.substr(first + 1, second - first - 1);
    message = rest.substr(second + 1);

    return !from.empty() && !to.empty();
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

void clientCommand(int clientSocket, std::vector<struct pollfd> *fds, const std::string &command)
{
    logMsg("Received from client " + std::to_string(clientSocket) + ": " + printable(command));

    if (command == "STATUSREQ")
    {
        if (!sendMessage(clientSocket, mailboxStatus()))
        {
            closeClient(clientSocket, fds);
        }
        return;
    }

    if (command.compare(0, 7, "GETMSG,") == 0)
    {
        std::string username;
        if (!parseGetMessage(command, username))
        {
            if (!sendMessage(clientSocket, "ERROR"))
            {
                closeClient(clientSocket, fds);
            }
            return;
        }

        auto &queue = mailboxes[username];
        if (queue.empty())
        {
            if (!sendMessage(clientSocket, "EMPTY"))
            {
                closeClient(clientSocket, fds);
            }
            return;
        }

        std::string stored = queue.front();
        queue.pop();
        if (queue.empty())
        {
            mailboxes.erase(username);
        }

        if (!sendMessage(clientSocket, "MSG," + stored))
        {
            closeClient(clientSocket, fds);
        }
        return;
    }

    if (command.compare(0, 8, "SENDMSG,") == 0)
    {
        std::string from;
        std::string to;
        std::string message;
        if (!parseSendMessage(command, from, to, message))
        {
            if (!sendMessage(clientSocket, "ERROR"))
            {
                closeClient(clientSocket, fds);
            }
            return;
        }

        mailboxes[to].push(from + ":" + message);
        if (!sendMessage(clientSocket, "OK"))
        {
            closeClient(clientSocket, fds);
        }
        return;
    }

    if (!sendMessage(clientSocket, "ERROR"))
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

        if (readyFds[0].revents & POLLIN)
        {
            int clientSock = accept(listenSock, (struct sockaddr *)&client, &clientLen);
            if (clientSock < 0)
            {
                perror("accept failed");
            }
            else
            {
                fds.push_back({clientSock, POLLIN, 0});
                clients[clientSock] = new Client(clientSock);
                logMsg("Client " + std::to_string(clientSock) + " connected from " +
                       inet_ntoa(client.sin_addr) + ":" + std::to_string(ntohs(client.sin_port)));
            }
        }

        for (size_t i = 1; i < readyFds.size(); ++i)
        {
            if (!(readyFds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            {
                continue;
            }

            const int sock = readyFds[i].fd;
            const ssize_t nread = recv(sock, buffer, sizeof(buffer), 0);
            if (nread <= 0)
            {
                closeClient(sock, &fds);
                continue;
            }

            auto it = clients.find(sock);
            if (it == clients.end())
            {
                continue;
            }

            std::string &inbuf = it->second->inbuf;
            std::string command;
            inbuf.append(buffer, static_cast<size_t>(nread));

            while (extractFrame(sock, inbuf, command))
            {
                clientCommand(sock, &fds, command);
            }
        }
    }

    close(listenSock);
    return 0;
}