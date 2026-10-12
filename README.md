## how to compile and run our code: 

Compile the code:
```
make
```

To run the tsam server:
./tsamserver <portnumber>

Example:
```
./tsamserver 4022
```

To run the tsam client:
./tsamclient <IPaddress> <portnumber>

Example:
```
./tsamclient 127.0.0.1 4022
```

## Operating system:
- We all are useing Linux although we all own windows computers 

## Commands:
SENDMSG, <from>, <to>, <message>



## How the server behaves in other situations
- Several clients: we implemented a poll loop so that the clients dont have to wait for each other and theres no busy waiting. 
- Message split or merged by TCP: each client has its own buffer and the cammends arent handled until the whole frame is avialable.
- Malformed input: these are the cases where the bytes get discarded: they bytes before SOH, a size smaller then 5 or over 5005 and  missing STX/ETX.
- Long commands: commands of up to 500 bytes are supported but larger ones are trashed
- A client disconnects: if they disconnect then the socket and buffere are cleaned up and its loggerd. the stored messages stay. since they belong to the usernames not the connections so they can be fetched after reconnecting. 
- Log: server.log gets all the commands recieded and sent, connects/disconnects and junk written with timestamps. They are also printed to the terminal. 

## Client behaviour
- One command per line and empty lines get skipped
- theres a printed timestamp with a UTC ISO 8601  with every command and recieved reply.
- junk from the server gets discarded

