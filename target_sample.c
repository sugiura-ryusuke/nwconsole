/* SPDX-License-Identifier: MIT */
/*
 * @file			target_sample.c
 * @brief			Target Sample for Network Console
 * @author			Sugiura Ryusuke <https://github.com/sugiura-ryusuke>
 * @date			2026/06/15
 *
 *
 * << How to build on Ubuntu / Cygwin >>
 *
 * $ gcc target_sample.c -o target_sample
 *
 *
 * << How to Use >>
 *
 *   [   PC  (IP address: 172.31.16.100)    ] <<==================>> [ Target Device (IP address: 172.31.16.200) ]
 *
 *   $ ./nwconsole -r 172.31.16.200                                  $ ./target_sample
 *
 *   (Change loglevel to TRACE)
 *    loglevel,T
 *
 *   (Change loglevel to FATAL)
 *    loglevel,F
 *
 *
 * - To specify a port number (default: 10000), use the -p option.
 *   $ ./target_sample -p 20000
 *
 * - To add a timestamp to the output, use the -t option.
 *   $ ./target_sample -t
 *
 * - To print debug message, use the -d option.
 *   $ ./target_sample -d
 *
 */

#include <stdarg.h>
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
		dbglog_output(DBGLOG_LEVEL_FATAL, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
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
		dbglog_output(DBGLOG_LEVEL_ERROR, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
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
		dbglog_output(DBGLOG_LEVEL_WARN, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
	} while(0)
#else
#define WARN_LOG(format, ...)
#endif

#if 1
#define INFO_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[INFO]  %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
		dbglog_output(DBGLOG_LEVEL_INFO, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
	} while(0)
#else
#define INFO_LOG(format, ...)
#endif

#if 1
#define DEBUG_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[DEBUG] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
		dbglog_output(DBGLOG_LEVEL_DEBUG, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
	} while(0)
#else
#define DEBUG_LOG(format, ...)
#endif

#if 1
#define TRACE_LOG(format, ...) \
	do { \
		if (debug) { \
			(void)printf("[TRACE] %s(%d) "format"\n", __func__, __LINE__, ##__VA_ARGS__); \
			(void)fflush(stdout); \
		} \
		dbglog_output(DBGLOG_LEVEL_TRACE, DBGLOG_APP, __func__, __LINE__, DBGLOG_ENCODE_NONE, format, ##__VA_ARGS__); \
	} while(0)
#else
#define TRACE_LOG(format, ...)
#endif

#define PRINT(format, ...) \
	do { \
		struct timespec ts; \
		struct tm *tm; \
		if (timestamp) { \
			(void)clock_gettime(CLOCK_REALTIME, &ts); \
			tm = gmtime(&ts.tv_sec); \
			if (tm != NULL) { \
				(void)printf("%04d-%02d-%02dT%02d:%02d:%02d.%03luZ ", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, ts.tv_nsec / 1000000); \
			} \
			else { \
				(void)printf("****-**-**T**:**:**.***Z "); \
			} \
		} \
		(void)printf(format, ##__VA_ARGS__); \
		(void)fflush(stdout); \
	} while(0)

/** maximum message size (recommended not to exceed the MTU size)  メッセージの最大長 */
#define MESSAGE_SIZE 1280

/** port number  ポート番号(1-65535) */
static uint16_t port_no = 10000;

/** Timestamp (RFC 3339 / ISO 8601)  タイムスタンプ */
static int timestamp = 0;

/* Debug output  デバッグ出力 */
static int debug = 0;

/** TCP socket  TCP通信用ソケット */
static int sock_client = -1;

/** process termination flag  プロセス停止フラグ */
static volatile sig_atomic_t process_termination = 0;

/** signal number that process received  プロセスが受信したシグナルの番号 */
static volatile sig_atomic_t process_signum = 0;

/** debug log level (based on Log4j)  デバッグログの出力レベル */
enum DBGLOG_LEVEL {
	DBGLOG_LEVEL_OFF,						/**< OFF: turn off logging */
	DBGLOG_LEVEL_FATAL,						/**< FATAL: severe errors that cause premature termination */
	DBGLOG_LEVEL_ERROR,						/**< ERROR: other runtime errors or unexpected conditions */
	DBGLOG_LEVEL_WARN,						/**< WARN: runtime situations that are undesirable or unexpected */
	DBGLOG_LEVEL_INFO,						/**< INFO: interesting runtime events */
	DBGLOG_LEVEL_DEBUG,						/**< DEBUG: detailed information on the flow through the system */
	DBGLOG_LEVEL_TRACE,						/**< TRACE: most detailed information */
	DBGLOG_LEVEL_NUM						/**< the number of level */
};

/** debug log facility code  デバッグログの機能コード */
enum DBGLOG_FACILITY {
	DBGLOG_APP,
	DBGLOG_FACILITY_NUM
};

/** debug log encode type  デバッグログのエンコード種別 */
enum DBGLOG_ENCODE {
	DBGLOG_ENCODE_NONE,						/**< do not encode */
	DBGLOG_ENCODE_CRLF,						/**< encode CR and LF into <CR> and <LF> */
	DBGLOG_ENCODE_HEX,						/**< Encode non-printable Characters into hex code */
	DBGLOG_ENCODE_NUM						/**< the number of encode type */
};

/** debug log level settings  デバッグログの出力レベル設定 */
static int dbglog_level[DBGLOG_FACILITY_NUM] = {
	DBGLOG_LEVEL_INFO,
};

/** command handler table  コマンドハンドラのテーブル */
typedef struct command_handler_table {
	const char *name;						/**< command name */
	int (*handler)(int argc, char *argv[]);	/**< handler function */
} command_handler_table_t;

static void signal_handler(int signum, siginfo_t *info, void *ucontext);
static int register_signal_handler(const int *signum, size_t count, void (*handler)(int sig, siginfo_t *info, void *ucontext), unsigned int flags);
static int create_timer(uint32_t time, int *p_sock);
static int create_ipv4_tcp_server_socket(struct in_addr addr, in_port_t port, int backlog, int *p_sock);
static int accept_ipv4_tcp_socket(int sock_listen, int *p_sock_connect, struct in_addr *p_addr, in_port_t *p_port);
static int receive_ipv4_tcp(int sock, void *buffer, size_t size, ssize_t *p_rsize);
static int send_ipv4_tcp(int sock, const void *data, size_t len, ssize_t *p_ssize);
static int wait_for_events(struct pollfd *fds, nfds_t nfds, int timeout);
static char *tokenize(char *str, const char *delim, char **saveptr);
static void dbglog_output(int level, int facility, const char *func, int linenum, int encode, const char* format, ...) __attribute__((format(printf, 6, 7)));
static int command_loglevel(int argc, char *argv[]);
static int parse_command(const char *message);
static void timer_event(uint32_t time);
static int receive_message_inet_tcp(int sock);
static int target_sample_main(void);
static void print_usage(void);
static int parse_opts(int argc, char *argv[], int *p_retcode);

static const command_handler_table_t command_handler_table[] = {
	{ "loglevel",		command_loglevel	},
	{ NULL, 			NULL				}
};

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
 * @brief create a IPv4 TCP server socket  IPv4プロトコルのTCPサーバーソケット生成
 *
 * @param[in]  addr     IPv4 address  IPv4アドレス
 * @param[in]  port     port number  ポート番号(1-65535)
 * @param[in]  backlog  the maximum length which the queue of pending connections for socket  ソケットへの接続待ちキューの最大長
 * @param[out] p_sock   TCP listener socket  TCPリスナーソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int create_ipv4_tcp_server_socket(struct in_addr addr, in_port_t port, int backlog, int *p_sock)
{
	int result = 0;
	int sock = -1;
	int reuse = 1;
	struct sockaddr_in s_in = { 0 };
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
		ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, (socklen_t)sizeof(reuse));
		if (ret != 0) {
			err = errno;
			ERROR_LOG("setsockopt(SO_REUSEADDR) failed. err(%d)", err);
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
			else {
				ret = listen(sock, backlog);
				if (ret != 0) {
					err = errno;
					ERROR_LOG("listen() failed. err(%d)", err);
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
 * @brief accept a connection on a IPv4 TCP server socket  IPv4プロトコルのTCPサーバーソケットへの接続受け付け
 *
 * @param[in]  sock_listen     TCP listener socket  TCPリスナーソケット
 * @param[out] p_sock_connect  TCP socket  TCP通信用ソケット
 * @param[out] p_addr          client IPv4 address  クライアントのIPv4アドレス
 * @param[out] p_port          port number  ポート番号(1-65535)
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int accept_ipv4_tcp_socket(int sock_listen, int *p_sock_connect, struct in_addr *p_addr, in_port_t *p_port)
{
	int result = 0;
	struct sockaddr_in s_in = { 0 };
	socklen_t s_in_len = sizeof(s_in);
	int sock;
	int err;

	if (p_sock_connect == NULL) {
		ERROR_LOG("p_sock_connect is NULL.");
		return -1;
	}

	sock = accept(sock_listen, (struct sockaddr*)&s_in, &s_in_len);
	if (sock == -1) {
		err = errno;
		switch (err) {
		case EAGAIN: /* Try again */
		case ENONET: /* Machine is not on the network */
		case EPROTO: /* Protocol error */
		case ENOPROTOOPT: /* Protocol not available */
		case EOPNOTSUPP: /* Operation not supported on transport endpoint */
		case ENETDOWN: /* Network is down */
		case ENETUNREACH: /* Network is unreachable */
		case EHOSTDOWN: /* Host is down */
		case EHOSTUNREACH: /* No route to host */
#if !defined(__linux__) && !defined(__CYGWIN__)
		case EWOULDBLOCK: /* Operation would block */
#endif
			/* do nothing */
			break;
		default:
			ERROR_LOG("accept() failed. err(%d)", err);
			result = -1;
			break;
		}
	}
	else {
		*p_sock_connect = sock;

		if (p_addr != NULL) {
			*p_addr = s_in.sin_addr;
		}
		if (p_port != NULL) {
			*p_port = s_in.sin_port;
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
 * @brief reentrant tokenization (replacement for strtok_r, can handle empty fields)  再入可能なトークン分割(strtok_rの空フィールド対応版)
 *
 * @param[in,out] str      point to the string to be parsed  トークン分割する文字列
 * @param[in]     delim    a set of bytes that delimit the tokens in the parsed string  トークン分割に使用するバイトの集合
 * @param[in,out] saveptr  a pointer to maintain the next token after next  次の次のトークンを保持するためのポインタ
 * @retutn a pointer to the next token, or NULL if there are no more tokens  次のトークンへのポインタ、トークンがなければNULL
 * @note  The contents of str are not preserved  文字列strの内容は保持されません
 */
static char *tokenize(char *str, const char *delim, char **saveptr)
{
	char *token;
	char *p;
	const char *d;

	if (delim == NULL) {
		return NULL;
	}
	if (saveptr == NULL) {
		return NULL;
	}

	token = (str != NULL) ? str : *saveptr;

	if ((token == NULL) || (*token == '\0')) {
		*saveptr = NULL;
	}
	else {
		for (p = token; *p != '\0'; p++) {
			for (d = delim; *d != '\0'; d++) {
				if (*p == *d) {
					break;
				}
			}
			if (*p == *d) {
				*p = '\0';
				p++;
				*saveptr = p;
				break;
			}
		}
		if (*saveptr != p) {
			*saveptr = NULL;
		}
	}

	return token;
}

/**
 * @brief output debug log message  デバッグログメッセージを出力
 *
 * @param[in] level     debug log output level  (DBGLOG_LEVEL)  デバッグログの出力レベル
 * @param[in] facility  facility code (DBGLOG_FACILITY)  種別コード
 * @param[in] func      function name  関数名
 * @param[in] linenum   linu number  行数
 * @param[in] encode    encode type (DBGLOG_ENCODE)  エンコード種別
 * @param[in] format    message format メッセージフォーマット
 * @return none  なし
 */
static void dbglog_output(int level, int facility, const char *func, int linenum, int encode, const char* format, ...)
{
	va_list ap;
	const char *prefix = "";
	const char *suffix = "\n";
	char buffer[MESSAGE_SIZE + 1] = { 0 };
	char buffer2[MESSAGE_SIZE + 1] = { 0 };
	char *tx_buffer = buffer;
	size_t len;
	size_t i;
	size_t k;
#if 0
	struct timespec ts = { 0 };
	struct tm tm = { 0 };
	struct tm *tm_ret;
	char time[128] = { 0 };
#endif

	if (func == NULL) {
		return;
	}
	if (format == NULL) {
		return;
	}

	if ((facility < 0) || (facility >= DBGLOG_FACILITY_NUM)) {
		return;
	}
	if (level > dbglog_level[facility]) {
		return;
	}

	switch (level) {
	case DBGLOG_LEVEL_FATAL:	prefix = "***** FATAL";	break;
	case DBGLOG_LEVEL_ERROR:	prefix = "****  ERROR";	break;
	case DBGLOG_LEVEL_WARN:		prefix = "***   WARN ";	break;
	case DBGLOG_LEVEL_INFO:		prefix = "**    INFO ";	break;
	case DBGLOG_LEVEL_DEBUG:	prefix = "*     DEBUG";	break;
	case DBGLOG_LEVEL_TRACE:	prefix = "      TRACE";	break;
	default:					prefix = "           ";	break;
	}

#if 0
	/* get system-wide real-time clock  システム全体の実時間(UTC)を取得 */
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	tm_ret = gmtime_r(&ts.tv_sec, &tm);
	if (tm_ret != NULL) {
		/* datetime(ISO 8601): xxxx-xx-xxTxx:xx:xx.xxxZ */
		(void)snprintf(time, sizeof(time), "%04d-%02d-%02dT%02d:%02d:%02d.%03luZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	}
	else {
		(void)snprintf(time, sizeof(time), "****-**-**T**:**:**.***Z");
	}

	(void)snprintf(buffer, sizeof(buffer), "%s %s %s(%d) ", prefix, time, func, linenum);
#else
	(void)snprintf(buffer, sizeof(buffer), "%s %s(%d) ", prefix, func, linenum);
#endif

	len = strlen(buffer);
	va_start(ap, format);
	(void)vsnprintf(&buffer[len], sizeof(buffer) - len, format, ap);
	va_end(ap);

	len = strlen(buffer);
	(void)snprintf(&buffer[len], sizeof(buffer) - len, "%s", suffix);

	len = strlen(buffer);
	switch (encode) {
	case DBGLOG_ENCODE_CRLF:
		for (i = 0, k = 0; i < len; i++) {
			if (buffer[i] == '\r') {
				if (k < sizeof(buffer2)) {
					buffer2[k] = '<';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = 'C';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = 'R';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = '>';
					k++;
				}
			}
			else if (buffer[i] == '\n') {
				if (k < sizeof(buffer2)) {
					buffer2[k] = '<';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = 'L';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = 'F';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = '>';
					k++;
				}
			}
			else {
				buffer2[k] = buffer[i];
				k++;
			}
		}
		buffer2[sizeof(buffer2) - 1] = '\0';
		tx_buffer = buffer2;
		break;

	case DBGLOG_ENCODE_HEX:
		for (i = 0, k = 0; i < len; i++) {
			if ((buffer[i] < 0x20) || (buffer[i] >= 0x7F)) {
				if (k < sizeof(buffer2)) {
					buffer2[k] = '<';
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = '0' + ((buffer[i] & 0xF0) >> 4) + ((((buffer[i] & 0xF0) >> 4) >= 10) ? 7 : 0);
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = '0' + (buffer[i] & 0x0F) + (((buffer[i] & 0x0F) >= 10) ? 7 : 0);
					k++;
				}
				if (k < sizeof(buffer2)) {
					buffer2[k] = '>';
					k++;
				}
			}
			else {
				buffer2[k] = buffer[i];
				k++;
			}
		}
		buffer2[sizeof(buffer2) - 1] = '\0';
		tx_buffer = buffer2;
		break;

	default:
		break;
	}

	if (sock_client != -1) {
		(void)send_ipv4_tcp(sock_client, tx_buffer, strlen(tx_buffer), NULL);
	}
}

/**
 * @brief loglevel command  loglevelコマンド処理
 *
 * @param[in]  argc  the number of command parameters  コマンドのパラメータの個数
 * @param[in]  argv  array of command parameters  コマンドのパラメータの集合
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int command_loglevel(int argc, char *argv[])
{
	int result = 0;
	int loglevel = 0;
	int i;

	if (argv == NULL) {
		return -1;
	}

	/*
	 * loglevel - set debug log output level  デバッグログの出力レベル設定
	 *   loglevel,[O/F/E/W/I/D/T],...
	 */
	for(i = 0; i < argc; i++) {
		if (argv[i] != NULL) {
			if (i < DBGLOG_FACILITY_NUM) {
				switch (argv[i][0]) {
				case 'O':	case '0':	loglevel = DBGLOG_LEVEL_OFF;	break;
				case 'F':	case '1':	loglevel = DBGLOG_LEVEL_FATAL;	break;
				case 'E':	case '2':	loglevel = DBGLOG_LEVEL_ERROR;	break;
				case 'W':	case '3':	loglevel = DBGLOG_LEVEL_WARN;	break;
				case 'I':	case '4':	loglevel = DBGLOG_LEVEL_INFO;	break;
				case 'D':	case '5':	loglevel = DBGLOG_LEVEL_DEBUG;	break;
				case 'T':	case '6':	loglevel = DBGLOG_LEVEL_TRACE;	break;
				default:				loglevel = -1;					break;
				}

				if ((loglevel >= DBGLOG_LEVEL_OFF) && (loglevel < DBGLOG_LEVEL_NUM)) {
					PRINT("*** Change log level(%d): %d ==> %d\n", i, dbglog_level[i], loglevel);
					dbglog_level[i] = loglevel;
				}
			}
		}
	}

	return result;
}

/**
 * @brief parse command sentence  コマンド文解析
 *
 * @param[in]  message  command sentence  コマンド文
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 */
static int parse_command(const char *message)
{
	int result = -1;
	char buffer[MESSAGE_SIZE + 1] = { 0 };
	char *str = buffer;
	char *token[16];
	char *saveptr = NULL;
	int count;
	const command_handler_table_t *p_table;

	(void)snprintf(buffer, sizeof(buffer), "%s", message);

	for (count = 0; count < 16; count++) {
		token[count] = tokenize(str, ",", &saveptr);
		if (token[count] == NULL) {
			break;
		}
		str = NULL;
	}

	for (p_table = command_handler_table; p_table->name != NULL; p_table++) {
		if (strcmp(token[0], p_table->name) == 0) {
			result = p_table->handler(count - 1, &token[1]);
			break;
		}
	}

	return result;
}

/**
 * @brief timer event handler  タイマーイベントハンドラ
 *
 * @param[in]  time  timer interval (in msec)  タイマー周期
 * @return none  なし
 */
static void timer_event(uint32_t time)
{
	static uint32_t timer_count_10sec = 0;

	timer_count_10sec += time;
	if (timer_count_10sec >= 10 * 1000) {
		timer_count_10sec = 0;
		FATAL_LOG("Timer 10sec.");
		ERROR_LOG("Timer 10sec.");
		WARN_LOG("Timer 10sec.");
		INFO_LOG("Timer 10sec.");
		DEBUG_LOG("Timer 10sec.");
		TRACE_LOG("Timer 10sec.");
	}
}

/**
 * @brief receive and display data over TCP socket  TCP通信用ソケットからデータを受信して表示
 *
 * @param[in]  sock  TCP socket  TCP通信用ソケット
 * @retval 0   success  成功
 * @retval -1  failure  失敗
 * @retval -2  disconneted  通信切断
 */
static int receive_message_inet_tcp(int sock)
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
					ret = parse_command(message);
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
 * @brief Target Sample main function  Target Sample のメイン関数
 *
 * @param[in]  none  なし
 * @retval EXIT_SUCCESS  success  成功
 * @retval EXIT_FAILURE  failure  失敗
 */
static int target_sample_main(void)
{
	int result = EXIT_SUCCESS;
	int main_thread_stop = 0;
	uint32_t timer_interval_ms = 500;
	int sock_timer = -1;
	int sock_listen = -1;
	struct pollfd fds[3] = { 0 };
	ssize_t rsize;
	uint64_t timer_count;
	struct in_addr client_addr = { INADDR_ANY };
	char client_addr_str[INET_ADDRSTRLEN];
	int ret;
	int err;

	ret = create_timer(timer_interval_ms, &sock_timer);
	if (ret == -1) {
		ERROR_LOG("create_timer() failed.");
		result = EXIT_FAILURE;
	}

	ret = create_ipv4_tcp_server_socket((struct in_addr){ INADDR_ANY }, htons(port_no), 1, &sock_listen);
	if (ret != 0) {
		ERROR_LOG("create_ipv4_tcp_server_socket() failed.");
		result = EXIT_FAILURE;
	}

	if (result == EXIT_SUCCESS) {
		fds[0].fd = sock_timer;
		fds[0].events = POLLIN;
		fds[1].fd = sock_listen;
		fds[1].events = POLLIN;
		fds[2].fd = -1;
		fds[2].events = 0;

		while (main_thread_stop == 0) {
			ret = wait_for_events(fds, 3, 1000);
			if (ret == -1) {
				break;
			}
			else if (ret > 0) {
				if ((fds[0].revents & POLLIN) != 0) {
					rsize = read(fds[0].fd, &timer_count, sizeof(timer_count));
					if (rsize == -1) {
						err = errno;
						ERROR_LOG("read() failed. err(%d)", err);
						result = -1;
					}
					else {
						timer_event(timer_interval_ms);
					}
				}
				if ((fds[1].revents & POLLIN) != 0) {
					sock_client = -1;
					ret = accept_ipv4_tcp_socket(fds[1].fd, &sock_client, &client_addr, NULL);
					if (ret != 0) {
						ERROR_LOG("accept_ipv4_tcp_socket() failed.");
					}
					else {
						if (sock_client != -1) {
							memset(client_addr_str, 0, sizeof(client_addr_str));
							(void)inet_ntop(AF_INET, &client_addr, client_addr_str, sizeof(client_addr_str));
							client_addr_str[INET_ADDRSTRLEN - 1] = '\0';
							PRINT("*** Connected. (%s)\n", client_addr_str);
							fds[1].fd = -1;
							fds[1].events = 0;
							fds[2].fd = sock_client;
							fds[2].events = POLLIN;
						}
					}
				}
				if ((fds[2].revents & POLLIN) != 0) {
					ret = receive_message_inet_tcp(fds[2].fd);
					if (ret == -1) {
						ERROR_LOG("receive_message_inet_tcp() failed.");
					}
					else if (ret == -2) {
						(void)close(sock_client);
						sock_client = -1;
						PRINT("*** Disconnected. (%s)\n", client_addr_str);
						fds[1].fd = sock_listen;
						fds[1].events = POLLIN;
						fds[2].fd = -1;
						fds[2].events = 0;
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

	if (sock_client != -1) {
		(void)close(sock_client);
	}
	if (sock_listen != -1) {
		(void)close(sock_listen);
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
	PRINT("Usage: target_sample [options]\n");
	PRINT("Options:\n");
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
	unsigned long port = 0;
	char *end = NULL;
	int c;

	const struct option opts[] = {
		/*	name			has_arg			flag			val			*/
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
		c = getopt_long(argc, argv, "p:td", opts, NULL);

		if (c == -1) {
			break;
		}

		switch (c) {
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
		result = target_sample_main();
	}

	return result;
}

