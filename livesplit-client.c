#include "livesplit-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSE_SOCKET(s) closesocket(s)
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
#define SOCK_ERRNO() WSAGetLastError()
#define SOCK_ERR_IN_PROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS || (e) == WSAEALREADY)
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef int socket_t;
#define CLOSE_SOCKET(s) close(s)
#define IS_INVALID_SOCKET(s) ((s) < 0)
#define INVALID_SOCKET (-1)
#define SOCK_ERRNO() errno
#define SOCK_ERR_IN_PROGRESS(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK || (e) == EALREADY)
#endif

#include <util/threading.h>
#include <util/platform.h>
#include <obs-module.h>

struct livesplit_client {
	char host[128];
	int port;
	livesplit_phase_changed_cb callback;
	void *param;

	pthread_t thread;
	os_event_t *stop_event;
	bool running;

	pthread_mutex_t mutex;
	livesplit_state_t state;
};

#ifdef _WIN32
static bool winsock_initialized = false;
static void init_winsock(void)
{
	if (!winsock_initialized) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		winsock_initialized = true;
	}
}
#else
static void init_winsock(void) {}
#endif

/* Non-blocking connect that can be aborted via stop_event. A blocking connect
 * to an unreachable host can stall for tens of seconds, which would freeze OBS
 * when livesplit_client_destroy() joins the worker thread. */
static bool connect_with_stop(socket_t sock, const struct sockaddr *addr, int addrlen, os_event_t *stop_event)
{
#ifdef _WIN32
	u_long nonblock = 1;
	ioctlsocket(sock, FIONBIO, &nonblock);
#else
	int old_flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, old_flags | O_NONBLOCK);
#endif

	bool connected = false;
	if (connect(sock, addr, addrlen) == 0) {
		connected = true;
	} else if (SOCK_ERR_IN_PROGRESS(SOCK_ERRNO())) {
		while (os_event_try(stop_event) == EAGAIN) {
			fd_set write_fds;
			FD_ZERO(&write_fds);
			FD_SET(sock, &write_fds);

			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 100000;

#ifdef _WIN32
			int sel = select(0, NULL, &write_fds, NULL, &tv);
#else
			int sel = select(sock + 1, NULL, &write_fds, NULL, &tv);
#endif
			if (sel > 0) {
				int so_error = 0;
#ifdef _WIN32
				int len = sizeof(so_error);
#else
				socklen_t len = sizeof(so_error);
#endif
				getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
				connected = (so_error == 0);
				break;
			} else if (sel < 0) {
				break;
			}
			/* sel == 0: timed out, loop to re-check stop_event */
		}
	}

#ifdef _WIN32
	nonblock = 0;
	ioctlsocket(sock, FIONBIO, &nonblock);
#else
	fcntl(sock, F_SETFL, old_flags);
#endif

	return connected;
}

static bool send_command(socket_t sock, const char *cmd, char *out_buf, size_t max_len)
{
	char send_buf[256];
	snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
#ifdef MSG_NOSIGNAL
	const int send_flags = MSG_NOSIGNAL;
#else
	const int send_flags = 0;
#endif
	int sent = send(sock, send_buf, (int)strlen(send_buf), send_flags);
	if (sent <= 0)
		return false;

	memset(out_buf, 0, max_len);
	int received = recv(sock, out_buf, (int)(max_len - 1), 0);
	if (received <= 0)
		return false;

	out_buf[received] = '\0';
	// Trim newline
	char *nl = strchr(out_buf, '\r');
	if (nl)
		*nl = '\0';
	nl = strchr(out_buf, '\n');
	if (nl)
		*nl = '\0';

	return true;
}

static livesplit_phase_t parse_phase(const char *str)
{
	if (!str || !*str)
		return LIVESPLIT_PHASE_NOT_RUNNING;
	if (strcmp(str, "Running") == 0)
		return LIVESPLIT_PHASE_RUNNING;
	if (strcmp(str, "Ended") == 0)
		return LIVESPLIT_PHASE_ENDED;
	if (strcmp(str, "Paused") == 0)
		return LIVESPLIT_PHASE_PAUSED;
	return LIVESPLIT_PHASE_NOT_RUNNING;
}

static void *livesplit_worker_thread(void *arg)
{
	livesplit_client_t *client = (livesplit_client_t *)arg;
	init_winsock();

	socket_t sock = INVALID_SOCKET;
	livesplit_phase_t last_phase = LIVESPLIT_PHASE_DISCONNECTED;

	while (os_event_try(client->stop_event) == EAGAIN) {
		if (IS_INVALID_SOCKET(sock)) {
			// Try to connect
			sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (!IS_INVALID_SOCKET(sock)) {
				// Set receive timeout to 1 second
#ifdef _WIN32
				DWORD timeout = 1000;
				setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
				setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
				struct timeval tv;
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
				setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv, sizeof(tv));
#endif
#ifdef SO_NOSIGPIPE
				/* macOS/BSD: don't let a write to a broken socket kill OBS. */
				int no_sigpipe = 1;
				setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

				struct addrinfo hints;
				struct addrinfo *result = NULL;
				memset(&hints, 0, sizeof(hints));
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_protocol = IPPROTO_TCP;

				char port_str[16];
				snprintf(port_str, sizeof(port_str), "%d", client->port);

				int gai = getaddrinfo(client->host, port_str, &hints, &result);
				if (gai != 0 || !result) {
					blog(LOG_WARNING, "[SpeedrunSourceRecord] Failed to resolve LiveSplit host '%s': %d",
					     client->host, gai);
					CLOSE_SOCKET(sock);
					sock = INVALID_SOCKET;
				} else {
					if (!connect_with_stop(sock, result->ai_addr,
							       (int)result->ai_addrlen, client->stop_event)) {
						CLOSE_SOCKET(sock);
						sock = INVALID_SOCKET;
					} else {
						blog(LOG_INFO,
						     "[SpeedrunSourceRecord] Connected to LiveSplit Server at %s:%d",
						     client->host, client->port);
					}
					freeaddrinfo(result);
				}
			}

			if (IS_INVALID_SOCKET(sock)) {
				if (last_phase != LIVESPLIT_PHASE_DISCONNECTED) {
					pthread_mutex_lock(&client->mutex);
					client->state.phase = LIVESPLIT_PHASE_DISCONNECTED;
					livesplit_state_t state_copy = client->state;
					pthread_mutex_unlock(&client->mutex);

					if (client->callback)
						client->callback(client->param, last_phase, LIVESPLIT_PHASE_DISCONNECTED, &state_copy);
					last_phase = LIVESPLIT_PHASE_DISCONNECTED;
				}
				// Sleep 1 second before retrying
				for (int i = 0; i < 10 && os_event_try(client->stop_event) == EAGAIN; i++) {
					os_sleep_ms(100);
				}
				continue;
			}
		}

		// Connected: query timer phase
		char response[256];
		if (!send_command(sock, "getcurrenttimerphase", response, sizeof(response))) {
			blog(LOG_WARNING, "[SpeedrunSourceRecord] Connection to LiveSplit lost.");
			CLOSE_SOCKET(sock);
			sock = INVALID_SOCKET;
			continue;
		}

		livesplit_phase_t current_phase = parse_phase(response);

		if (current_phase != last_phase) {
			char game_buf[256] = {0};
			char cat_buf[256] = {0};
			char attempt_buf[64] = {0};

			send_command(sock, "getgamename", game_buf, sizeof(game_buf));
			send_command(sock, "getcategoryname", cat_buf, sizeof(cat_buf));
			send_command(sock, "getattemptcount", attempt_buf, sizeof(attempt_buf));

			pthread_mutex_lock(&client->mutex);
			client->state.phase = current_phase;
			if (strlen(game_buf) > 0)
				snprintf(client->state.game, sizeof(client->state.game), "%s", game_buf);
			if (strlen(cat_buf) > 0)
				snprintf(client->state.category, sizeof(client->state.category), "%s", cat_buf);
			if (strlen(attempt_buf) > 0)
				client->state.attempt_count = atoi(attempt_buf);

			livesplit_state_t state_copy = client->state;
			pthread_mutex_unlock(&client->mutex);

			blog(LOG_INFO, "[SpeedrunSourceRecord] LiveSplit phase changed: %d -> %d (Game: '%s', Cat: '%s', Run #%d)",
			     last_phase, current_phase, state_copy.game, state_copy.category, state_copy.attempt_count);

			if (client->callback)
				client->callback(client->param, last_phase, current_phase, &state_copy);

			last_phase = current_phase;
		}

		// Wait 100ms before next poll
		for (int i = 0; i < 2 && os_event_try(client->stop_event) == EAGAIN; i++) {
			os_sleep_ms(50);
		}
	}

	if (!IS_INVALID_SOCKET(sock)) {
		CLOSE_SOCKET(sock);
	}

	return NULL;
}

livesplit_client_t *livesplit_client_create(const char *host, int port, livesplit_phase_changed_cb cb, void *param)
{
	livesplit_client_t *client = (livesplit_client_t *)bzalloc(sizeof(livesplit_client_t));
	if (!client)
		return NULL;

	snprintf(client->host, sizeof(client->host), "%s", host && *host ? host : "127.0.0.1");
	client->port = port > 0 ? port : 16834;
	client->callback = cb;
	client->param = param;
	client->state.phase = LIVESPLIT_PHASE_DISCONNECTED;
	client->state.attempt_count = 1;

	pthread_mutex_init(&client->mutex, NULL);
	os_event_init(&client->stop_event, OS_EVENT_TYPE_MANUAL);

	client->running = true;
	if (pthread_create(&client->thread, NULL, livesplit_worker_thread, client) != 0) {
		os_event_destroy(client->stop_event);
		pthread_mutex_destroy(&client->mutex);
		bfree(client);
		return NULL;
	}

	return client;
}

void livesplit_client_destroy(livesplit_client_t *client)
{
	if (!client)
		return;

	if (client->running) {
		os_event_signal(client->stop_event);
		pthread_join(client->thread, NULL);
		client->running = false;
	}

	os_event_destroy(client->stop_event);
	pthread_mutex_destroy(&client->mutex);
	bfree(client);
}

void livesplit_client_get_state(livesplit_client_t *client, livesplit_state_t *out_state)
{
	if (!client || !out_state)
		return;

	pthread_mutex_lock(&client->mutex);
	*out_state = client->state;
	pthread_mutex_unlock(&client->mutex);
}
