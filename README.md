Name: Mirza Ana-Maria

Group: 321CA

# Asyncronous Web Server

The purpose of the homework was the implementation of a server using advanced operations such as aio operations, multiplexing using epoll, non-blocking sockets, and zero-copying. 

The web server comunicates with clients using the http protocol, with the help of a http parser. The client makes a http request for a file and the server provides a http request followed by the content of the file, if the requested file was found. The connection is then closed by the server.

For the files requested by the clients, the server has two directories: a 'static/' folder containing files and a 'dynamic/' folder.

* files found in the 'static' folder are sent using the zero-copying API (sendfile) with a 'HTTP 200 OK' message

*  files found in the 'dynamic' folder are sent using asyncronous API with a 'HHTP 200 OK' message

* files that are not found in either folders return a 'HTTP 404 Not Found' message


## Implementation 

For the implementation, the server creates a connection structure for each new connection and uses the filed 'status' along with offset variables in order to determine the status of the connection: whether the http client message was received fully, or whether the reply message was sent completely. Therefore, the server can continue its action from the point it was paused.

The epoll implementation takes care of the multiplexing, making sure that the server receives new connections and serves the already present ones when their turn comes.

### Receiving Messages
The connection is placed in the outevents of epoll only after the whole message was received from the client. The partially received message is first stored in the recv_buffer, then at the end of the send_buffer of the connection. In this manner, at the end of the reading from the client socket, the whole message is available and the connection is placed in the out events of epoll. By searchin for '\r\n\r\n' at the end of the http message, the server determines if the whole message was received.

### Sending Messages - static files
For the sending of the static files, a zero-copying mechanism is used sing the sendfile() function. Since the sockets are non-blocking and the function does not send all the file at once, the connection structure uses internal offset variables to keep count of the bytes sent so that when the connection is scheduled by epoll again, it can resume by sending again data from the right offset. When sendfile reached the end of the file and the bytes sent are 0, the server closes the connection and removes it from epoll.

### Sending Messages - dynamic files
The asyncronous part for the dynamic files was implemented using the aio functions provided by the library "libaio.h".

As a summary, I am dividing the total size of the file into chuncks manageable by a normal sized buffer and create that number of aio operations. As expected, not all events are submitted at once, therefore I need to keep count of them and resubmit after sending all aio buffers finished. When receiving notitification of events finished, I wait for them using aio_getevent() and submit for sending to the client. After having submitted all events and having them finished and sent to the client, connection is closed and aio structures destroyed.

## Feedback
I believe the homework is useful the understading of sockets, but the lack of labs (except for the zero-copying one) made it really hard to understand what the homework requested and how does each part work (epoll, aio, sendfile, http, ..).

## How to Compile and Run

The aws.c source file containing the server links with the http_parser.h , aws.h, w_epoll.h, util.h, and sockutil.h extern libraries. For debuging purposes, it also also links with debug.h library. 

To run build the executable the following command in linux bash:
```
make
```

To start the server and put in listen mode use:
```
./aws
```

The server now listens on port 8888. To send client requests, open another terminal and send a http reply of a folder as in the following exemple:
```
$ nc localhost 8888
GET /static/file.dat HTTP/1.0
```
Or
```
echo -ne "GET /dynamic/file.dat HTTP/1.0\r\n\r\n" | nc -q 2 localhost 8888
```


## Bibliography

* https://ocw.cs.pub.ro/courses/so/laboratoare/laborator-11
* https://www.man7.org/linux/man-pages/man1/man.1.html
* https://open-education-hub.github.io/operating-systems/Lab/I/O/Zero-Copy/content/zero-copy
* https://open-education-hub.github.io/operating-systems/Lab/I/O/Asynchronous%20I/O/content/async-io
* https://open-education-hub.github.io/operating-systems/Lab/I/O/I/O%20Multiplexing/content/io-multiplexing
* The Linux Programming Interface - Ch. 61, 63
