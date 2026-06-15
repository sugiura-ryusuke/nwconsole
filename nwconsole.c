/* SPDX-License-Identifier: MIT */
/*
 * @file			nwconsole.c
 * @brief			Network Console
 * @author			Sugiura Ryusuke <https://github.com/sugiura-ryusuke>
 * @date			2026/06/15
 *
 *
 * << How to build on Ubuntu / Cygwin >>
 *
 * $ gcc nwconsole.c -o nwconsole
 *
 *
 * << How to Use >>
 *
 * (Case 1) interact with a target device over the network
 *
 *   [   PC  (IP address: 172.31.16.100)    ] <<==================>> [ Target Device (IP address: 172.31.16.200) ]
 *
 *   $ ./nwconsole -r 172.31.16.200                                  $ nc -l -p 10000
 *
 *
 * (Case 2) logging interaction with a target device over the network.
 *
 *   [   PC  (IP address: 172.31.16.100)    ] <<==================>> [ Target Device (IP address: 172.31.16.200) ]
 *
 *   $ ./nwconsole -r 172.31.16.200 >> log.txt &                     $ nc -l -p 10000
 *
 *   $ echo "abc" | nc -u -w 1 127.0.0.1 10000
 *
 *
 * - Text entered via standard input are output to the other host's standard output.
 *
 * - To specify a port number (default: 10000), use the -p option.
 *   $ ./nwconsole -r 172.31.16.200 -p 20000
 *
 * - To add a timestamp to the output, use the -t option.
 *   $ ./nwconsole -r 172.31.16.200 -t
 *
 * - To print debug message, use the -d option.
 *   $ ./nwconsole -r 172.31.16.200 -d
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if 1
#define FATAL_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[FATAL] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define FATAL_LOG(format, ...)
#endif

#if 1
#define ERROR_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[ERROR] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define ERROR_LOG(format, ...)
#endif

#if 1
#define WARN_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[WARN]  %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define WARN_LOG(format, ...)
#endif

#if 0
#define INFO_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[INFO]  %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define INFO_LOG(format, ...)
#endif

#if 0
#define DEBUG_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[DEBUG] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define DEBUG_LOG(format, ...)
#endif

#if 0
#define TRACE_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[TRACE] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
	} while(0)
#else
#define TRACE_LOG(format, ...)
#endif

#define PRINT(fmt, ...) \
	do { \
		struct timespec ts; \
		struct tm *tm; \
		if (timestamp) { \
			(void)clock_gettime(CLOCK_REALTIME, &ts); \
			tm = gmtime(&ts.tv_sec); \
			if (tm != NULL) { \
				(void)printf("%04d-%02d-%02dT%02d:%02d:%02d.%03luZ "fmt, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, ts.tv_nsec / 1000000, ##__VA_ARGS__); \
			} \
			else { \
				(void)printf("****-**-**T**:**:**.***Z "fmt, ##__VA_ARGS__); \
			} \
		} \
		else { \
			(void)printf(fmt, ##__VA_ARGS__); \
		} \
		(void)fflush(stdout); \
	} while(0)

/** maximum message size (recommended not to exceed the MTU size)  メッセージの最大長 */
#define MESSAGE_SIZE 1280

/** remote IP address  クライアントモードで動作する際の接続先アドレス */
static struct in_addr remote_addr = { INADDR_ANY };

/** port number  ポート番号(1-65535) */
static uint16_t port_no = 10000;

/** Timestamp (RFC 3339 / ISO 8601)  タイムスタンプ */
static int timestamp = 0;

/* Debug output  デバッグ出力 */
static int debug = 0;

/** process termination flag  プロセス停止フラグ */
static volatile sig_atomic_t process_termination = 0;

/** signal number that process received  プロセスが受信したシグナルの番号 */
static volatile sig_atomic_t process_signum = 0;

static void signal_handler(int signum, siginfo_t *info, void *ucontext);
static int register_signal_handler(const int *signum, size_t count, void (*handler)(int sig, siginfo_t *info, void *ucontext), unsigned int flags);
static int create_timer(uint32_t time, int *p_sock);
static int create_ipv4_tcp_client_socket(struct in_addr addr, in_port_t port, int timeout, int *p_sock);
static int receive_ipv4_tcp(int sock, void *buffer, size_t size, ssize_t *p_rsize);
static int send_ipv4_tcp(int sock, const void *data, size_t len, ssize_t *p_ssize);
static int create_ipv4_udp_socket(struct in_addr addr, in_port_t port, int *p_sock);
static int wait_for_events(struct pollfd *fds, nfds_t nfds, int timeout);
static int send_file_to_inet_tcp(int fd, int sock);
static int send_received_inet_udp_to_inet_tcp(int sock_udp, int sock_tcp);
static int print_received_inet_tcp(int sock);
static int nwconsole_main(void);
static void print_usage(void);
static int parse_opts(int argc, char *argv[], int *p_retcode);

/**
 * @brief the signal-handling function  シグナルハンドラ
 *
 * @param[in] signum    signal number of signal  シグナルの番号
 * @param[in] info      siginfo_t structure  siginfo_t構造体へのポインタ
 * @param[in] ucontext  ucontext_t structure  ucontext_t構造体へのポインタ
 * @return none  なし
 */
static void signal_handler(int signum, siginfo_t *info __attribute__((__unused__)), void *ucontext __attribute__((__unused__)))
{
	/* SIG30-C. Call only asynchronous-safe functions within signal handlers  */

	/* SIG31-C. Do not access shared objects in signal handlers */
	if (process_termination == 0) {
		process_termination = 1;
		process_signum = (sig_atomic_t)signum;
	}
}

/**
 * @brief register a signal handler  シグナルハンドラの登録
 *
 * @param[in] signum   the list of signals (except SIGKILL and SIGSTOP)  シグナルの集合
 * @param[in] count    the number of the signals  シグナルの個数
 * @param[in] handler  the signal-handling function for signum  シグナルハンドラ
 * @param[in] flags    a set of flags which modify the behavior of the signal  シグナルハンドラの動作を規定するフラグ
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int register_signal_handler(const int *signum, size_t count, void (*handler)(int sig, siginfo_t *info, void *ucontext), unsigned int flags)
{
	int result = 0;
	struct sigaction sa = { 0 };
	size_t i;
	int ret;
	int err;

	if (signum == NULL) {
		ERROR_LOG("signum is NULL.");
		return -1;
	}

	sa.sa_sigaction = handler;
	sa.sa_flags = (int)(SA_SIGINFO | flags);

	ret = sigemptyset(&sa.sa_mask);
	if (ret != 0) {
		err = errno;
		ERROR_LOG("sigemptyset() failed. err(%d)", err);
		result = -1;
	}
	else {
		for (i = 0; i < count; i++) {
			ret = sigaction(signum[i], &sa, NULL);
			if (ret != 0) {
				err = errno;
				ERROR_LOG("sigaction() failed. signum(%d) err(%d)", signum[i], err);
				result = -1;
			}
		}
	}

	return result;
}

/**
 * @brief create a timer descriptor  タイマーディスクリプタ生成
 *
 * @param[in]  time    interval time (in ms)  タイマ―の周期起動時間(ミリ秒)
 * @param[out] p_sock  timer descriptor       タイマーディスクリプタ
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int create_timer(uint32_t time, int *p_sock)
{
	int result = 0;
	int sock = -1;
	uint32_t sec;
	uint32_t msec;
	time_t tv_sec;
	long tv_nsec;
	struct itimerspec new_value;
	int ret;
	int err;

	if (p_sock == NULL) {
		ERROR_LOG("p_sock is NULL.");
		return -1;
	}

	sock = timerfd_create(CLOCK_MONOTONIC, 0);
	if (sock == -1) {
		err = errno;
		ERROR_LOG("timerfd_create() failed. (%d)", err);
		result = -1;
	}
	else {
		sec = time / 1000;
		msec = time - sec * 1000;

		tv_sec = (time_t)sec;
		tv_nsec = (long)(msec * 1000000);

		new_value.it_interval.tv_sec = tv_sec;
		new_value.it_interval.tv_nsec = tv_nsec;
		new_value.it_value.tv_sec = tv_sec;
		new_value.it_value.tv_nsec = tv_nsec;

		ret = timerfd_settime(sock, 0, &new_value, NULL);
		if (ret != 0) {
			err = errno;
			ERROR_LOG("timerfd_settime() failed. (%d)", err);
			result = -1;
		}

		if (result == 0) {
			*p_sock = sock;
		}
		else {
			(void)close(sock);
		}
	}

	return result;
}

/**
 * @brief create a IPv4 TCP client socket and connect to a server  IPv4プロトコルのTCPクライアントソケットを生成してサーバーへ接続
 *
 * @param[in]  addr     remote IPv4 address  接続先のIPv4アドレス
 * @param[in]  port     port number  ポート番号(1-65535)
 * @param[in]  timeout  waiting time for connection in milliseconds  接続待ち時間(ミリ秒)
 * @param[out] p_sock   TCP socket  TCP通信用ソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 * @note Specifying a negative value in timeout means an infinite timeout  timeoutに負数を指定した場合はタイムアウトなし
 */
static int create_ipv4_tcp_client_socket(struct in_addr addr, in_port_t port, int timeout, int *p_sock)
{
	int result = 0;
	int sock = -1;
	struct sockaddr_in s_in = { 0 };
	int flags;
	struct pollfd fds = { 0 };
	int so_error = 0;
	socklen_t so_error_len = sizeof(so_error);
	int ret;
	int err;

	if (p_sock == NULL) {
		ERROR_LOG("p_sock is NULL.");
		return -1;
	}

	sock = socket(AF_INET, (int)SOCK_STREAM, 0);
	if (sock == -1) {
		err = errno;
		ERROR_LOG("socket() failed. errno(%d)", err);
		result = -1;
	}
	else {
		/* F_GETFL, F_SETFL - get/set file status flags
		 *   cf.) https://man7.org/linux/man-pages/man2/f_getfl.2const.html
		 */
		flags = fcntl(sock, F_GETFL);

		ret = fcntl(sock, F_SETFL, (flags | O_NONBLOCK));
		if (ret != 0) {
			err = errno;
			ERROR_LOG("fcntl(F_SETFL, O_NONBLOCK) failed. err(%d)", err);
			result = -1;
		}

		if (result == 0) {
			s_in.sin_family = AF_INET;
			s_in.sin_addr = addr;
			s_in.sin_port = port;

			ret = connect(sock, (struct sockaddr*)&s_in, sizeof(s_in));
			if (ret != 0) {
				err = errno;
				if (err == EINPROGRESS) { /* Operation now in progress (non-blocking mode) */
					fds.fd = sock;
					fds.events = POLLOUT;
					ret = poll(&fds, 1, timeout);
					switch (ret) {
					case -1:
						err = errno;
						if (err == EINTR) {
							INFO_LOG("poll() failed by interrupted system call.");
						}
						else {
							ERROR_LOG("poll() failed. err(%d)", err);
						}
						result = -1;
						break;

					case 0:
						/* poll() timed out */
						TRACE_LOG("poll() timed out.");
						result = -1;
						break;

					default:
						ret = getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
						if (ret != 0) {
							err = errno;
							ERROR_LOG("getsockopt(SO_ERROR) failed. err(%d)", err);
							result = -1;
						}
						else {
							if (so_error != 0) {
								if (so_error == ECONNREFUSED) { /* Connection refused */
									INFO_LOG("getsockopt(SO_ERROR): Connection refused.");
								}
								else {
									WARN_LOG("getsockopt(SO_ERROR): reason(%d)", so_error);
								}
								result = -1;
							}
						}
						break;
					}
				}
				else if (err == ECONNREFUSED) { /* Connection refused */
					INFO_LOG("connect() failed. Connection refused.");
					result = -1;
				}
				else {
					ERROR_LOG("connect() failed. err(%d)", err);
					result = -1;
				}
			}

			if (result == 0) {
				ret = fcntl(sock, F_SETFL, flags);
				if (ret != 0) {
					err = errno;
					ERROR_LOG("fcntl(F_SETFL) failed. err(%d)", err);
					result = -1;
				}
			}
		}

		if (result == 0) {
			*p_sock = sock;
		}
		else {
			(void)close(sock);
		}
	}

	return result;
}

/**
 * @brief receive data over IPv4 TCP socket  IPv4プロトコルのTCP通信用ソケットからデータを受信
 *
 * @param[in]  sock     TCP socket  TCP通信用ソケット
 * @param[out] buffer   receive buffer  受信バッファ
 * @param[in]  size     receive buffer size  受信バッファのサイズ
 * @param[out] p_rsize  the actual size of the received data  実際に受信したデータのサイズ
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 * @retval -2  connection reset by peer  相手側による通信切断
 * @note the value of p_rsize is 0 when disconneted  接続切断を検知した場合はp_rsizeに0の値が返る
 */
static int receive_ipv4_tcp(int sock, void *buffer, size_t size, ssize_t *p_rsize)
{
	int result = 0;
	ssize_t rsize;
	int err;

	if (buffer == NULL) {
		ERROR_LOG("buffer is NULL.");
		return -1;
	}

	rsize = recv(sock, buffer, size, 0);
	if (rsize == -1) {
		err = errno;
		if (err == ECONNRESET) { /* Connection reset by peer */
			INFO_LOG("recv() failed, a peer sent RST packet.");
			result = -2;
		}
		else {
			ERROR_LOG("recv() failed. err(%d)", err);
			result = -1;
		}
	}
	else {
		if (p_rsize != NULL) {
			*p_rsize = rsize;
		}
	}

	return result;
}

/**
 * @brief send data over IPv4 TCP socket  IPv4プロトコルのTCP通信用ソケットからデータを送信
 *
 * @param[in]  sock     TCP socket  TCP通信用ソケット
 * @param[in]  data     the data to be transmitted  送信するデータ
 * @param[in]  len      length of the data to be transmitted  送信するデータのサイズ
 * @param[out] p_ssize  the actual size of the data sent  実際に送信したデータのサイズ
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int send_ipv4_tcp(int sock, const void *data, size_t len, ssize_t *p_ssize)
{
	int result = 0;
	ssize_t ssize;
	int err;

	if (data == NULL) {
		ERROR_LOG("data is NULL.");
		return -1;
	}

	ssize = send(sock, data, len, 0);
	if (ssize == -1) {
		err = errno;
		ERROR_LOG("send() failed. err(%d)", err);
		result = -1;
	}
	else {
		if (p_ssize != NULL) {
			*p_ssize = ssize;
		}
	}

	return result;
}

/**
 * @brief create a IPv4 UDP socket  IPv4プロトコルのUDPソケット生成
 *
 * @param[in]  addr    IPv4 address  IPv4アドレス
 * @param[in]  port    port number  ポート番号(1-65535)
 * @param[out] p_sock  UDP socket  UDPソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int create_ipv4_udp_socket(struct in_addr addr, in_port_t port, int *p_sock)
{
	int result = 0;
	int sock = -1;
	struct sockaddr_in s_in = { 0 };
	int ret;
	int err;

	if (p_sock == NULL) {
		ERROR_LOG("p_sock is NULL.");
		return -1;
	}

	sock = socket(AF_INET, (int)SOCK_DGRAM, 0);
	if (sock == -1) {
		err = errno;
		ERROR_LOG("socket() failed. errno(%d)", err);
		result = -1;
	}
	else {
		s_in.sin_family = AF_INET;
		s_in.sin_addr = addr;
		s_in.sin_port = port;

		ret = bind(sock, (struct sockaddr*)&s_in, sizeof(s_in));
		if (ret != 0) {
			err = errno;
			ERROR_LOG("bind() failed. err(%d)", err);
			result = -1;
		}

		if (result == 0) {
			*p_sock = sock;
		}
		else {
			(void)close(sock);
		}
	}

	return result;
}

/**
 * @brief wait for poll event  pollイベント待ち
 *
 * @param[in]  fds      the set of file descriptors  ファイルディスクリプタの集合
 * @param[in]  nfds     the number of file descriptors  ファイルディスクリプタの個数
 * @param[in]  timeout  timeout value in milliseconds  タイムアウト値(ミリ秒)
 * @retval >0  the number of elements in fds whose revents fields have been set  イベントが発生したディスクリプタの個数
 * @retval 0   timeout  タイムアウト
 * @retval -1  failure  失敗
 */
static int wait_for_events(struct pollfd *fds, nfds_t nfds, int timeout)
{
	int result = 0;
	int ret;
	int err;

	if (fds == NULL) {
		ERROR_LOG("fds is NULL.");
		return -1;
	}

	ret = poll(fds, nfds, timeout);
	switch (ret) {
	case -1:
		err = errno;
		if (err == EINTR) {
			INFO_LOG("poll() failed by interrupted system call.");
		}
		else {
			ERROR_LOG("poll() failed. err(%d)", err);
			result = -1;
		}
		break;

	case 0:
		/* poll() timed out */
		TRACE_LOG("poll() timed out.");
		break;

	default:
		result = ret;
		break;
	}

	return result;
}

/**
 * @brief send a file data over TCP socket  ファイルのデータをTCP通信用ソケットから送信
 *
 * @param[in]  fd    file descriptor  ファイルディスクリプタ
 * @param[in]  sock  TCP socket  TCP通信用ソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int send_file_to_inet_tcp(int fd, int sock)
{
	int result = 0;
	uint8_t buffer[MESSAGE_SIZE + 1] = { 0 };
	ssize_t rsize;
	size_t len;
	int ret;
	int err;

	rsize = read(fd, buffer, sizeof(buffer) - 1);
	if (rsize < 0) {
		err = errno;
		ERROR_LOG("read() failed. err(%d)", err);
		result = -1;
	}
	else {
		len = strlen((char*)buffer);
		if ((len > 0) && (len < sizeof(buffer))) {
			/*
			 *  before                                after
			 * { 'A',  'B',  'C',  '\n',  '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 * { 'A',  'B',  '\r', '\n',  '\0' } ==> { 'A',  'B',  '\n', '\0',  '\0' }
			 * { 'A',  'B',  'C',  '\0',  '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 * { 'A',  'B',  'C',  'D',   '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 */
			if (buffer[len - 1] == '\n') {
				if ((len >= 2) && (buffer[len - 2] == '\r')) {
					buffer[len - 2] = '\n';
					buffer[len - 1] = '\0';
				}
			}
			else {
				if (len < sizeof(buffer) - 1) {
					buffer[len] = '\n';
				}
				else {
					buffer[len - 1] = '\n';
				}
			}

			if (sock != -1) {
				ret = send_ipv4_tcp(sock, buffer, strlen((char*)buffer), NULL);
				if (ret != 0) {
					ERROR_LOG("send_ipv4_tcp() failed.");
					result = -1;
				}
			}
		}
	}

	return result;
}

/**
 * @brief receive a UDP packet over IPv4  IPv4プロトコルでUDPパケットを受信
 *
 * @param[in]  sock_udp  UDP socket  UDPソケット
 * @param[in]  sock_tcp  TCP socket  TCP通信用ソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int send_received_inet_udp_to_inet_tcp(int sock_udp, int sock_tcp)
{
	int result = 0;
	uint8_t buffer[MESSAGE_SIZE + 1] = { 0 };
	ssize_t rsize;
	size_t len;
	int ret;
	int err;

	rsize = recvfrom(sock_udp, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
	if (rsize == -1) {
		err = errno;
		ERROR_LOG("recvfrom() failed. err(%d)", err);
		result = -1;
	}
	else {
		len = strlen((char*)buffer);
		if ((len > 0) && (len < sizeof(buffer))) {
			/*
			 *  before                                after
			 * { 'A',  'B',  'C',  '\n',  '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 * { 'A',  'B',  '\r', '\n',  '\0' } ==> { 'A',  'B',  '\n', '\0',  '\0' }
			 * { 'A',  'B',  'C',  '\0',  '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 * { 'A',  'B',  'C',  'D',   '\0' } ==> { 'A',  'B',  'C',  '\n',  '\0' }
			 */
			if (buffer[len - 1] == '\n') {
				if ((len >= 2) && (buffer[len - 2] == '\r')) {
					buffer[len - 2] = '\n';
					buffer[len - 1] = '\0';
				}
			}
			else {
				if (len < sizeof(buffer) - 1) {
					buffer[len] = '\n';
				}
				else {
					buffer[len - 1] = '\n';
				}
			}

			PRINT("%s", buffer);

			if (sock_tcp != -1) {
				ret = send_ipv4_tcp(sock_tcp, buffer, strlen((char*)buffer), NULL);
				if (ret != 0) {
					ERROR_LOG("send_ipv4_tcp() failed.");
					result = -1;
				}
			}
		}
	}

	return result;
}

/**
 * @brief receive and display data over TCP socket  TCP通信用ソケットからデータを受信して表示
 *
 * @param[in]  sock  TCP socket  TCP通信用ソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 * @retval -2  disconneted  通信切断
 */
static int print_received_inet_tcp(int sock)
{
	int result = 0;
	uint8_t buffer[MESSAGE_SIZE + 1] = { 0 };
	ssize_t rsize = 0;
	size_t len;
	size_t i;
	size_t k;
	int ret;

	static char message[MESSAGE_SIZE + 1] = { 0 };
	static int enable = 1;

	ret = receive_ipv4_tcp(sock, buffer, sizeof(buffer) - 1, &rsize);
	if (ret == -1) {
		ERROR_LOG("receive_ipv4_tcp() failed.");
		result = -1;
	}
	else if ((ret == -2) || (rsize == 0)) {
		result = -2;
	}
	else {
		len = strlen((char*)buffer);
		k = strlen(message);

		for (i = 0; i < len; i++) {
			if ((char)buffer[i] == '\n') {
				if (k > 0) {
					PRINT("%s\n", message);
					memset(message, 0, sizeof(message));
					k = 0;
				}
				enable = 1;
			}
			else {
				if (enable) {
					if (k < sizeof(message) - 1) {
						message[k] = (char)buffer[i];
						k++;
					}
					else {
						WARN_LOG("message is too long.");
						memset(message, 0, sizeof(message));
						k = 0;
						enable = 0;
					}
				}
			}
		}
	}

	return result;
}

/**
 * @brief Network Console main function  Network Console のメイン関数
 *
 * @param[in]  none  なし
 * @retval EXIT_SUCCESS  success  成功
 * @retval EXIT_FAILURE  failure  失敗
 */
static int nwconsole_main(void)
{
	int result = EXIT_SUCCESS;
	int main_thread_stop = 0;
	int sock_timer = -1;
	int sock_udp = -1;
	int sock_tcp = -1;
	struct pollfd fds[4] = { 0 };
	ssize_t rsize;
	uint64_t timer_count;
	int ret;
	int err;

	ret = create_timer(500, &sock_timer);
	if (ret == -1) {
		ERROR_LOG("create_timer() failed.");
		result = EXIT_FAILURE;
	}

	ret = create_ipv4_udp_socket((struct in_addr){ INADDR_ANY }, htons(port_no), &sock_udp);
	if (ret != 0) {
		ERROR_LOG("create_ipv4_udp_socket() failed.");
		result = EXIT_FAILURE;
	}

	if (result == EXIT_SUCCESS) {
		fds[0].fd = fileno(stdin);
		fds[0].events = POLLIN;
		fds[1].fd = sock_timer;
		fds[1].events = POLLIN;
		fds[2].fd = sock_udp;
		fds[2].events = POLLIN;
		fds[3].fd = -1;
		fds[3].events = 0;

		while (main_thread_stop == 0) {
			ret = wait_for_events(fds, 4, 1000);
			if (ret == -1) {
				break;
			}
			else if (ret > 0) {
				if ((fds[0].revents & POLLIN) != 0) {
					ret = send_file_to_inet_tcp(fds[0].fd, sock_tcp);
					if (ret != 0) {
						ERROR_LOG("send_file_to_inet_tcp() failed.");
					}
				}
				if ((fds[1].revents & POLLIN) != 0) {
					rsize = read(fds[1].fd, &timer_count, sizeof(timer_count));
					if (rsize == -1) {
						err = errno;
						ERROR_LOG("read() failed. err(%d)", err);
						result = -1;
					}
					else {
						if (sock_tcp == -1) {
							ret = create_ipv4_tcp_client_socket(remote_addr, htons(port_no), 100, &sock_tcp);
							if (ret == 0) {
								PRINT("*** Connected.\n");
								fds[3].fd = sock_tcp;
								fds[3].events = POLLIN;
							}
						}
					}
				}
				if ((fds[2].revents & POLLIN) != 0) {
					ret = send_received_inet_udp_to_inet_tcp(fds[2].fd, sock_tcp);
					if (ret != 0) {
						ERROR_LOG("send_received_inet_udp_to_inet_tcp() failed.");
					}
				}
				if ((fds[3].revents & POLLIN) != 0) {
					ret = print_received_inet_tcp(fds[3].fd);
					if (ret == -1) {
						ERROR_LOG("print_received_inet_tcp() failed.");
					}
					else if (ret == -2) {
						(void)close(sock_tcp);
						sock_tcp = -1;
						PRINT("*** Disconnected.\n");
						fds[3].fd = -1;
						fds[3].events = 0;
					}
					else {
						/* do nothing */
					}
				}
			}
			else {
				/* do nothing */
			}

			if (process_termination != 0) {
				INFO_LOG("signal received. signum(%d)", process_signum);
				main_thread_stop = 1;
			}
		}
	}

	if (sock_tcp != -1) {
		(void)close(sock_tcp);
	}
	if (sock_udp != -1) {
		(void)close(sock_udp);
	}
	if (sock_timer != -1) {
		(void)close(sock_timer);
	}

	return result;
}

/**
 * @brief show details of command-line options  コマンドラインオプションの詳細表示
 *
 * @param[in]  none  なし
 * @return none  なし
 */
static void print_usage(void)
{
	PRINT("Usage: nwconsole [options]\n");
	PRINT("Options:\n");
	PRINT("  -r, --remote=<addr>   Remote IP address\n");
	PRINT("  -p, --port=<number>   Port number (1-65535)\n");
	PRINT("  -t, --timestamp       Timestamp (RFC 3339 / ISO 8601)\n");
	PRINT("  -d, --debug           Debug output\n");
	PRINT("      --help            Show this message and quit\n");
}

/**
 * @brief parse command-line options  コマンドラインオプション解析
 *
 * @param[in]  argc  the number of command-line arguments  コマンドライン引数の個数
 * @param[in]  argv  array of command-line argument strings  コマンドライン引数の集合
 * @param[out] p_retcode  return code of process  プロセスのリターンコード
 * @retval 0   execute process  プロセス実行
 * @retval -1  terminate process  プロセス終了
 */
static int parse_opts(int argc, char *argv[], int *p_retcode)
{
	int result = 0;
	struct in_addr addr;
	unsigned long port = 0;
	char *end = NULL;
	int c;
	int ret;

	const struct option opts[] = {
		/*	name			has_arg			flag			val			*/
		{	"remote",		1,				NULL,			'r'			},
		{	"port",			1,				NULL,			'p'			},
		{	"timestamp",	0,				NULL,			't'			},
		{	"debug",		0,				NULL,			'd'			},
		{	"help",			0,				NULL,			'h'			},
		{	NULL,			0,				NULL,			0			}
	};

	if (p_retcode == NULL) {
		return -1;
	}

	while (1) {
		c = getopt_long(argc, argv, "r:p:td", opts, NULL);

		if (c == -1) {
			break;
		}

		switch (c) {
		case 'r':
			/* remote IP address */
			ret = inet_pton(AF_INET, optarg, &addr);
			if (ret != 1) {
				ERROR_LOG("remote(%s) is not a IPv4 address.", optarg);
				result = -1;
			}
			else {
				remote_addr = addr;
			}
			break;

		case 'p':
			/* Port number (1-65535) */
			port = strtoul(optarg, &end, 10);
			if ((end != NULL) && (*end != '\0')) {
				ERROR_LOG("port(%s) is not a number.", optarg);
				result = -1;
			}
			else if ((port < 1) || (port > 65535)) {
				ERROR_LOG("option 'port' be between 1 and 65535.");
				result = -1;
			}
			else {
				port_no = (uint16_t)port;
			}
			break;

		case 't':
			/* Timestamp */
			timestamp = 1;
			break;

		case 'd':
			/* Debug output */
			debug = 1;
			break;

		case 'h':
			/* Help */
			print_usage();
			*p_retcode = EXIT_SUCCESS;
			result = -1;
			break;

		default:
			print_usage();
			*p_retcode = EXIT_FAILURE;
			result = -1;
			break;
		}

		if (result != 0) {
			break;
		}
	}

	return result;
}

/**
 * @brief main function  メイン関数
 *
 * @param[in]  argc  the number of command-line arguments  コマンドライン引数の個数
 * @param[in]  argv  array of command-line argument strings  コマンドライン引数の集合
 * @retval EXIT_SUCCESS  success  成功
 * @retval EXIT_FAILURE  failure  失敗
 */
int main(int argc, char *argv[])
{
	int result = EXIT_SUCCESS;
	const int signum[3] = { SIGINT, SIGPIPE, SIGTERM };
	int ret;

	ret = parse_opts(argc, argv, &result);
	if (ret != 0) {
		return result;
	}

	ret = register_signal_handler(signum, 3, signal_handler, SA_RESTART);
	if (ret != 0) {
		ERROR_LOG("register_signal_handler() failed.");
		result = EXIT_FAILURE;
	}
	else {
		result = nwconsole_main();
	}

	return result;
}

