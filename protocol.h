//
// Shared code for the TSAM mail client and server.
//
// Every message on the wire looks like:
//
//    <SOH><length><STX><command><ETX>
//
// where <length> is a 16-bit unsigned integer in network byte order giving
// the length of the whole frame in bytes, including the 5 framing bytes.

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <cstddef>

const char SOH = 0x01;                   // Start of header
const char STX = 0x02;                   // Start of text
const char ETX = 0x03;                   // End of text

const size_t FRAME_OVERHEAD = 5;         // <SOH> + 2 length bytes + <STX> + <ETX>
const size_t MAX_FRAME_LEN  = 5000;      // Longest frame we accept

// Result of trying to pull one frame out of a receive buffer.
enum class FrameResult
{
    Complete,       // A whole frame was removed from the buffer
    Incomplete,     // Need more bytes before a frame can be read
    Malformed       // Garbage or an invalid frame was discarded
};

// Wrap a command in <SOH><length><STX>...<ETX>.
std::string frameMessage(const std::string &command);

// Try to take one frame off the front of "buffer".
// Call repeatedly until it returns Incomplete.
FrameResult extractFrame(std::string &buffer, std::string &command);

// Send all of "data" on "sock". Returns false if the connection is broken.
bool sendAll(int sock, const std::string &data);

// Current time as "YYYY-MM-DD HH:MM:SS".
std::string timestamp();

#endif
