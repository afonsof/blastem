#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CTL_CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CTL_CLOSE close
#endif
#include "control.h"
#include "blastem.h"
#include "util.h"
#include "version.inc"

int control_enabled = 0;
static int control_sock = -1;
static char rx[4096];
static int rx_len = 0;

void control_send_event(const char *json_body)
{
	if (control_sock < 0) return;
	char line[1024];
	int n = snprintf(line, sizeof(line), "{%s}\n", json_body);
	send(control_sock, line, n, 0);
}

static void control_send_ok(const char *result_body)
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "{\"ok\":true,\"result\":{%s}}\n",
		result_body ? result_body : "");
	send(control_sock, line, n, 0);
}

static void control_send_err(const char *msg)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	send(control_sock, line, n, 0);
}

// Read one newline-terminated request line into out (without the newline).
// Returns 1 on success, 0 on disconnect.
static int control_read_line(char *out, int cap)
{
	for (;;) {
		for (int i = 0; i < rx_len; i++) {
			if (rx[i] == '\n') {
				int len = i < cap - 1 ? i : cap - 1;
				memcpy(out, rx, len);
				out[len] = 0;
				// strip trailing CR
				if (len && out[len-1] == '\r') out[len-1] = 0;
				memmove(rx, rx + i + 1, rx_len - i - 1);
				rx_len -= i + 1;
				return 1;
			}
		}
		int space = (int)sizeof(rx) - rx_len;
		if (space <= 0) { rx_len = 0; space = sizeof(rx); }
		int got = recv(control_sock, rx + rx_len, space, 0);
		if (got <= 0) return 0;
		rx_len += got;
	}
}

int control_active(void) { return control_sock >= 0; }

void control_init(int port)
{
#ifdef _WIN32
	WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
	int lsock = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		fatal_error("control: bind on port %d failed\n", port);
	}
	if (listen(lsock, 1) < 0) {
		fatal_error("control: listen failed\n");
	}
	control_sock = accept(lsock, NULL, NULL);
	CTL_CLOSE(lsock);
	if (control_sock < 0) {
		fatal_error("control: accept failed\n");
	}
	control_enabled = 1;
	char hello[128];
	int n = snprintf(hello, sizeof(hello),
		"{\"event\":\"hello\",\"version\":\"%s\",\"protocol\":1}\n", BLASTEM_VERSION);
	send(control_sock, hello, n, 0);
}

// Dispatch a single request line. Returns:
//   0 = handled inline (reply already sent), stay paused
//   (Tasks 8-10 extend this for step/run/pause/pad/screenshot/reset)
int control_dispatch(char *line)
{
	if (!strcmp(line, "ping")) {
		control_send_ok("\"pong\":true");
		return 0;
	}
	if (!strcmp(line, "version")) {
		char body[128];
		snprintf(body, sizeof(body), "\"version\":\"%s\",\"protocol\":1", BLASTEM_VERSION);
		control_send_ok(body);
		return 0;
	}
	control_send_err("unknown command");
	return 0;
}

// Read+dispatch commands until a command asks the emulator to advance
// (step/run) or the client disconnects. Tasks 8+ give control_dispatch a
// nonzero return to break this loop.
void control_pause_loop(void *sys)
{
	char line[2048];
	while (control_sock >= 0) {
		if (!control_read_line(line, sizeof(line))) {
			CTL_CLOSE(control_sock);
			control_sock = -1;
			exit(0);
		}
		if (control_dispatch(line)) return;
	}
}
