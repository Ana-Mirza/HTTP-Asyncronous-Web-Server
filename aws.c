/*
 * epoll-based echo server. Uses epoll(7) to multiplex connections.
 *
 * TODO:
 *  - block data receiving when receive buffer is full (use circular buffers)
 *  - do not copy receive buffer into send buffer when send buffer data is
 *      still valid
 *
 * 2011-2017, Operating Systems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/sendfile.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "aws.h"
#include "http_parser.h"

char http_error[BUFSIZ] = "HTTP/1.0 404 File Not Found\r\n"
		"Date: Sun, 14 January 2023 13:08:16 GMT\r\n"
		"Last-Modified:  Mon, 01 December 2023 14:30:27 GMT\r\n"
		"Accept-Ranges: bytes\r\n"
		"Content-Length:\r\n"
		"Vary: Accept-Encoding\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n"
		"\r\n";

/* http parser */
static http_parser request_parser;
static char request_path[BUFSIZ];	/* storage for request_path */

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_HEADER_SENDING,
	STATE_DATA_SENDING,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED,
	STATE_CONNECTION_CREATED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;

	int fd;
	struct stat *buf;

	/* offset in file to be sent */
	size_t file_offset;
	/* total size of file */
	size_t file_size;
    /* bytes parsed by http parser */
    size_t bytes_parsed;
    /* buffer for requested file path */
    char request_path[BUFSIZ];
	/* buffers used for receiving and seding messages */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	size_t send_offset;
	enum connection_state state;
};

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->state = STATE_CONNECTION_CREATED;
	conn->sockfd = sockfd;
	conn->fd = -2;
	conn->buf = malloc(sizeof(struct stat));
	conn->send_offset = 0;
	conn->send_len = 0;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	return conn;
}

/*
 * Copy receive buffer to send buffer (echo).
 */

static void connection_copy_buffers(struct connection *conn)
{
	memcpy(conn->send_buffer + conn->send_len, conn->recv_buffer, conn->recv_len);
	conn->send_len += conn->recv_len;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	if (conn->fd >= 0) {
		close(conn->fd);
	}
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn->buf);
	free(conn);
	dlog(LOG_INFO, "Connection closed\n");
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* make socket non-blocking*/
	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

receive:
	bytes_recv = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}

	/* update status */
	conn->recv_len = bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Sends dynamic file asyncronously.
 */
void send_dynamic_file(struct connection *conn) {
	return;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	/* check if connection is in sending process */
	if (conn->state == STATE_HEADER_SENDING) {
		goto send_header;
	} else if (conn->state == STATE_DATA_SENDING) {
		goto send_file;
	}

	/* reset send buffer */
	memset(conn->send_buffer, 0, BUFSIZ);

	/* set http reply in send buffer */
	if (conn->fd == -1) {
		conn->send_len = strlen(http_error);
		memcpy(conn->send_buffer, http_error, conn->send_len);
		goto send_header;
	} else {
		/* find size of file */
		fstat(conn->fd, conn->buf);
		conn->file_size = conn->buf->st_size;

		/* valid file http reply */
		char buffer[BUFSIZ];
		int rc = sprintf(buffer, "HTTP/1.1 200 OK\r\n"
		"Date: Sun, 14 January 2023 13:08:16 GMT\r\n"
		"Server: Apache/2.2.9\r\n"
		"Last-Modified: Mon, 01 December 2023 14:30:27 GMT\r\n"
		"Accept-Ranges: bytes\r\n"
		"Content-Length: %ld\r\n"
		"Vary: Accept-Encoding\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n"
		"\r\n", conn->file_size);

		conn->send_len = strlen(buffer);
		memcpy(conn->send_buffer, buffer, conn->send_len);
	}

send_header:
	/* send http reply */
	bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_offset, conn->send_len, 0);
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	/* update offsets */
	conn->send_offset += bytes_sent;
	conn->send_len -= bytes_sent;

	/* check if header was sent */
	if (conn->send_len) {
		conn->state = STATE_HEADER_SENDING;
		return STATE_HEADER_SENDING;
	}

send_file:
	/* send static file */
	if (strstr(conn->request_path, "static") != NULL && conn->fd != -1) {

		/* set number of bytes to be send */
		int nr_bytes;
		if (conn->file_size > BUFSIZ) {
			nr_bytes = BUFSIZ;
		} else {
			nr_bytes = conn->file_size;
		}

		/* zero-copying */
		ssize_t bytes_sent;
		bytes_sent = sendfile(conn->sockfd, conn->fd, (off_t *) &conn->file_offset, nr_bytes);

		/* update offset and bytes to be copyed */
		conn->file_size -= (size_t) bytes_sent;
		conn->state = STATE_DATA_SENDING;
		
		/* remove connection if whole file was sent */
		if (!bytes_sent) {
			conn->state = STATE_DATA_SENT;
			goto remove_connection;
		} else {
			return STATE_DATA_SENDING;
		}

	} else if (conn->fd != -1) {
		send_dynamic_file(conn);
	}

	conn->state = STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

static void get_requested_file(struct connection *conn) {
    /* init HTTP_REQUEST parser */
	http_parser_init(&request_parser, HTTP_REQUEST);

	conn->bytes_parsed = http_parser_execute(&request_parser, &settings_on_path,
                                        conn->send_buffer, strlen(conn->send_buffer));
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;

	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	/* check if whole message was received*/
	if (!strstr(conn->recv_buffer, "\r\n\r\n")) {
		connection_copy_buffers(conn);
		return;
	}

	/* save requested path */
	connection_copy_buffers(conn);
	get_requested_file(conn);
	memset(conn->request_path, 0, BUFSIZ);
	sprintf(conn->request_path, "%s%s", ".", request_path);

	/* open file */
	conn->fd = open(conn->request_path, O_RDWR);

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_message(rev.data.ptr);
			}
		}
	}

	return 0;
}
