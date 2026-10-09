# TSAM A6 – Mail server and client

## The big picture

```
  ┌────────────┐        TCP         ┌────────────┐
  │ tsamclient │ ─────────────────► │ tsamserver │
  │            │ ◄───────────────── │            │
  └────────────┘   framed messages  └────────────┘
   keyboard in                        server.log
   timestamped out                    mailboxes
```

Several clients can be connected to one server at the same time.

## What a message looks like

```
 ┌─────┬──────────┬─────┬──────────────────┬─────┐
 │ SOH │ length   │ STX │ command          │ ETX │
 │ 0x01│ 2 bytes  │ 0x02│ e.g. STATUSREQ   │ 0x03│
 └─────┴──────────┴─────┴──────────────────┴─────┘
   length = whole frame including the 5 framing bytes, max 5000
```

TCP can split or merge messages, so both sides keep a buffer and only
handle a command once the whole frame has arrived.

## Files

```
protocol.h / .cpp   shared: frame, unframe, timestamp
server.cpp          tsamserver
client.cpp          tsamclient
Makefile            "make" builds both
```

## Server idea

```
            poll() waits (no busy-waiting)
                     │
     ┌───────────────┼────────────────┐
     ▼               ▼                ▼
 new client     data from client   client left
  accept         add to buffer      close + clean up
                     │
               whole frame?
                     │
              handle command ──► reply + log
```

Data the server keeps:

```
buffers    socket   ──► bytes received so far
mailboxes  username ──► [ msg, msg, msg ]   (oldest first)
```

## Commands

| Command                         | What happens                              |
|---------------------------------|-------------------------------------------|
| `SENDMSG,<from>,<to>,<message>` | store message for `<to>`                  |
| `GETMSG,<username>`             | send oldest message for user, delete it   |
| `STATUSREQ`                     | `user1:2,user2:1` or `EMPTY`              |
| anything else                   | error reply, logged, no crash             |

## Client idea

```
      poll() on keyboard + socket
          │                 │
     user typed         server sent
     frame + send       unframe + print
     print with time    print with time
```

## Plan

1. Framing (protocol)
2. Server loop: connect / read / disconnect
3. Server commands + log
4. Client
5. Test: many clients, reconnecting, garbage with `ncat`
6. Record demo: `trace.pcapng`, `server.log`, `client-output.txt`
7. Finish this README (how to build/run, OS, behaviour)
