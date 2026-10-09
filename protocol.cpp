//
// Shared code for the TSAM mail client and server.
// See protocol.h for a description of the frame format.

#include "protocol.h"

#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

std::string frameMessage(const std::string &command)
{
    // - Return "" if command + FRAME_OVERHEAD is longer than MAX_FRAME_LEN
    // - Build: SOH, 2 length bytes (htons), STX, command, ETX
}

FrameResult extractFrame(std::string &buffer, std::string &command)
{
    // - Empty buffer                         -> Incomplete
    // - First byte not SOH                   -> drop up to next SOH, Malformed
    // - Fewer than 4 bytes                   -> Incomplete
    // - Byte 3 not STX, or length bad        -> drop the SOH, Malformed
    // - Fewer than <length> bytes            -> Incomplete
    // - Last byte of frame not ETX           -> drop the SOH, Malformed
    // - Otherwise copy out the command, remove the frame, Complete
}

bool sendAll(int sock, const std::string &data)
{
    // - Loop send() until everything is sent (use MSG_NOSIGNAL)
    // - Retry on EINTR, return false on any other error
}

std::string timestamp()
{
    // - time() + localtime_r() + strftime("%Y-%m-%d %H:%M:%S")
}
