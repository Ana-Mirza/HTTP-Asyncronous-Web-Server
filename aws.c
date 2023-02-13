
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
#include <libaio.h>
#include <sys/eventfd.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "aws.h"
#include "http_parser.h"

#define EVENTS 1

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
	STATE_HEADER_SENT,
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

	/* aio dependencies */
	struct iocb **piocb;
	struct iocb *iocb;
	char **aio_buf;
	size_t num_aio;
	io_context_t ctx;
	int efd;
	int num_submitted;
	int num_aio_finished;
	int total_aio_op;
	int aio_sent;
	int last_bytes;

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
};

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

/* Add eventfd to epoll */
int w_epoll_add_efd(int epollfd, int fd, void *ptr)
{
	struct epoll_event ev;

	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = ptr;
	return epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
}

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
	conn->num_aio_finished = 0;
	conn->aio_sent = 0;
	conn->num_submitted = 0;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	conn->efd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->efd < 0, "eventfd");

	/* create ctx */
	io_setup(EVENTS, &conn->ctx);

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
	/* destroy context */
	int rc = io_destroy(conn->ctx);
	DIE(rc < 0, "io_destroy");

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

/* Read file from the disk asyncronously
 */
void read_file_aio(struct connection *conn) {

	/* total number of aio operations */
	int n = conn->file_size / BUFSIZ;
	if (conn->file_size % BUFSIZ)
		n++;

	conn->total_aio_op = n;
	/* submit all operations */
	if (conn->num_submitted < n && conn->num_submitted) {
		goto submit;
	} else if (conn->num_submitted == n) {
		goto end;
	}

	/* alloc structures */
	conn->iocb = malloc(n * sizeof(struct iocb));
	conn->piocb = malloc(n * sizeof(struct iocb *));
	conn->aio_buf = malloc(n * sizeof(char*));
	if (!conn->iocb || !conn->piocb || !conn->aio_buf) {
		dlog(LOG_ERR, "malloc error");
		connection_remove(conn);
	}

	size_t count;
	for (int i = 0; i < n; i++) {
		conn->piocb[i] = &conn->iocb[i];
		conn->aio_buf[i] = malloc(BUFSIZ * sizeof(char));

		/* number of bytes to read in buf */
		count = BUFSIZ;
		if (i == n - 1 && conn->file_size % BUFSIZ != 0) {
			count = conn->file_size % BUFSIZ;
		}

		io_prep_pread(&conn->iocb[i], conn->fd, conn->aio_buf[i], count,
					conn->file_offset);
		io_set_eventfd(&conn->iocb[i], conn->efd);

		/* update offsets */
		conn->file_offset += count;
	}
	
	conn->last_bytes = count;

	/* remove from out pool */
	int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

submit:
	rc = io_submit(conn->ctx, n - conn->num_submitted, conn->piocb + conn->num_submitted);
	DIE(rc < 0, "io_submit");

	conn->num_submitted += rc;

	if (!conn->num_aio_finished) {
		/* add evetfd in epoll */
		rc = w_epoll_add_efd(epollfd, conn->efd, conn);
		DIE(rc < 0, "w_epoll_add_efd");
	}

end:
	return;
}

/* Collect finished aio operations.
 */
void collect_aio(struct connection *conn) {
	/* catch finished operations */
	struct io_event events[conn->num_submitted];
	u_int64_t eval;

	int rc = read(conn->efd, &eval, sizeof(eval));
	DIE(rc < 0, "read");

	/* submit for sending */
	rc = io_getevents(conn->ctx, eval, eval, events, NULL);
	DIE(rc != eval, "io_getevents");
	conn->num_aio_finished += eval;

	/* add to epollout */
	rc = w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_out");
}

/*
 * Sends dynamic file asyncronously.
 */

void send_dynamic_file(struct connection *conn) {
	if (conn->aio_sent == conn->num_aio_finished)
		read_file_aio(conn);
	conn->state = STATE_DATA_SENDING;

	/* check if buffer is sending */
	if (conn->send_len)
		goto sending_file;

	if (!conn->num_aio_finished)
		return;

	if (conn->num_aio_finished > conn->aio_sent) {

		/* number of bytes to be sent */
		int bytes;
		if (conn->aio_sent == conn->num_submitted - 1) {
			bytes = conn->last_bytes;
		} else {
			bytes = BUFSIZ;
		}
		memcpy(conn->send_buffer, conn->aio_buf[conn->aio_sent], bytes);
		conn->send_len = bytes;

		/* info about connection */
		char abuffer[64];

		int rc = get_peer_address(conn->sockfd, abuffer, 64);
		if (rc < 0) {
			ERR("get_peer_address");
			goto remove_connection;
		}

sending_file:
		; /* send file */
		int bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_offset, conn->send_len, 0);
		if (bytes_sent < 0) {		/* error in communication */
			dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
			goto remove_connection;
		}
		if (bytes_sent == 0) {		/* connection closed */
			dlog(LOG_ERR, "Connection closed to %s\n", abuffer);
			goto remove_connection;
		}

		/* update offsets */
		conn->send_offset += bytes_sent;
		conn->send_len -= bytes_sent;

		/* check if data was sent */
		if (conn->send_len) {
			conn->state = STATE_DATA_SENDING;
			return;
		}

		conn->send_offset = 0;
		conn->aio_sent++;

		/* finished seding all ready buffers */
		if (conn->aio_sent == conn->num_aio_finished) {

			/* remove from out notifications */
			int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
			DIE(rc < 0, "w_epoll_remove_ptr");

			/* submit more events if needed */
			if (conn->num_aio_finished != conn->total_aio_op)
				read_file_aio(conn);
			return;
		}
	}

	/* finished sending all buffers - close connection */
	if (conn->aio_sent == conn->total_aio_op) {
		dlog(LOG_ERR, "All aio buffers sent - closing connection!");
remove_connection:
		for (int i = 0; i < conn->total_aio_op; i++) {
			free(conn->aio_buf[i]);
		}
		free(conn->aio_buf);

		int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		/* remove current connection */
		connection_remove(conn);
		return;
	}

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
		sprintf(buffer, "HTTP/1.1 200 OK\r\n"
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

	conn->state = STATE_HEADER_SENT;
	conn->send_offset = 0;

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
		return STATE_DATA_SENDING;
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

	/* deal with aio operations */
	if (conn->state == STATE_DATA_SENDING) {
		collect_aio(conn);
		return;
	}

	/* receive http message from client */
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
