//
// Simple mail client for TSAM assignment A6.
//
// Usage: ./tsamclient <ip> <port>

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
#include <vector>
#include <poll.h>

#include <iostream>
#include <sstream>
#include <ctime>

#define SOH 0x01
#define STX 0x02
#define ETX 0x03
#define FRAME_OVERHEAD 5
#define MAX_FRAME 5000

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

void printTimestamped(const std::string &label, const std::string &text)
{
    std::cout << "[" << timestamp() << "] " << label << text << std::endl;
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

bool extractFrame(std::string &buf, std::string &command)
{
    while (!buf.empty())
    {
        size_t start = buf.find(static_cast<char>(SOH));
        if (start == std::string::npos)
        {
            buf.clear();
            return false;
        }
        if (start > 0)
        {
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
            buf.erase(0, 1);
            continue;
        }
        if (buf.size() < len)
        {
            return false;
        }
        if (buf[3] != STX || buf[len - 1] != ETX)
        {
            buf.erase(0, 1);
            continue;
        }

        command = buf.substr(4, len - FRAME_OVERHEAD);
        buf.erase(0, len);
        return true;
    }
    return false;
}

void sendFramedCommand(int serverSocket, const std::string &command)
{
    const std::string msg = frame(command);
    const char *p = msg.c_str();
    size_t remaining = msg.length();

    printTimestamped("Sent: ", printable(command));
    while (remaining > 0)
    {
        ssize_t n = send(serverSocket, p, remaining, 0);
        if (n <= 0)
        {
            std::cerr << "[" << timestamp() << "] send() failed\n";
            exit(1);
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
}

std::string trimLine(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    {
        s.pop_back();
    }
    return s;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: ./tsamclient <ip> <port>\n");
        printf("Ctrl-C to terminate\n");
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *serverInfo = nullptr;
    if (getaddrinfo(argv[1], argv[2], &hints, &serverInfo) != 0)
    {
        perror("getaddrinfo failed");
        return 1;
    }

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        perror("socket failed");
        freeaddrinfo(serverInfo);
        return 1;
    }

    if (connect(serverSocket, serverInfo->ai_addr, serverInfo->ai_addrlen) != 0)
    {
        perror("connect failed");
        close(serverSocket);
        freeaddrinfo(serverInfo);
        return 1;
    }
    freeaddrinfo(serverInfo);

    printTimestamped("Connected to ", std::string(argv[1]) + ":" + argv[2]);

    std::vector<struct pollfd> fds;
    fds.push_back({STDIN_FILENO, POLLIN, 0});
    fds.push_back({serverSocket, POLLIN, 0});

    std::string inbuf;
    char readBuffer[4096];

    while (true)
    {
        int n = poll(fds.data(), fds.size(), -1);
        if (n < 0)
        {
            perror("poll failed");
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP))
        {
            if ((fds[0].revents & POLLHUP) && !(fds[0].revents & POLLIN))
            {
                break;
            }

            char line[2048];
            if (fgets(line, sizeof(line), stdin) == nullptr)
            {
                break;
            }

            std::string command = trimLine(std::string(line));
            if (command.empty())
            {
                continue;
            }

            sendFramedCommand(serverSocket, command);
        }

        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
        {
            ssize_t nread = recv(serverSocket, readBuffer, sizeof(readBuffer), 0);
            if (nread <= 0)
            {
                printTimestamped("Server disconnected", "");
                break;
            }

            inbuf.append(readBuffer, static_cast<size_t>(nread));
            std::string command;
            while (extractFrame(inbuf, command))
            {
                printTimestamped("Received: ", printable(command));
            }
        }
    }

    close(serverSocket);
    return 0;
}
