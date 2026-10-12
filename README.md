# TSAM A6 – Client-Server Messaging

This project implements a simple TCP mailbox server and command-line client for the TSAM assignment.

## Development environment

- Developed and tested on Ubuntu 22.04 inside WSL, with the code built using the standard Linux socket API and GCC.
- The project is expected to compile on Linux without third-party networking libraries.

## Build

From the project root, run:

```bash
make
```

This builds:

- `./tsamserver`
- `./tsamclient`

## Run the server

```bash
./tsamserver 4044
```

The server listens on the TCP port passed as the first command-line argument. It keeps a timestamped `server.log` and handles multiple clients concurrently using `poll()`.

## Run the client

```bash
./tsamclient 127.0.0.1 4044
```

Once connected, the client accepts commands from standard input; each line is framed and sent to the server. Incoming frames are unframed and printed with a timestamp.

## Commands supported

The protocol uses the required framed format:

```text
<SOH><length><STX><command><ETX>
```

The server understands these commands:

- `SENDMSG,<from>,<to>,<message>`
  - Stores one message for the destination user.
  - The message is queued in FIFO order for that recipient.

- `GETMSG,<username>`
  - Returns the oldest queued message for that user and deletes it.
  - If no message exists, the server replies with `EMPTY`.

- `STATUSREQ`
  - Returns a fixed format reply listing each user with queued messages and the count:
    - example: `alice:2,bob:1`
    - empty mailbox: `EMPTY`

Malformed frames and unknown commands are logged and rejected with an `ERROR` reply instead of crashing the server.

## Behaviour notes

- TCP is treated as a byte stream, so both client and server buffer incoming bytes until a complete framed message is available.
- The server keeps one FIFO queue per user.
- Clients may connect and disconnect repeatedly without the server crashing or losing state for other users.
- The server avoids busy-waiting by blocking in `poll()` while idle.

## Example session

```text
./tsamserver 4044
./tsamclient 127.0.0.1 4044
SENDMSG,alice,bob,hello
STATUSREQ
GETMSG,bob
```

Expected responses are framed and will appear as messages such as:

```text
OK
bob:1
MSG,alice:hello
```
