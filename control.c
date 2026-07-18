#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CTL_CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
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
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

static void control_send_ok(const char *result_body)
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "{\"ok\":true,\"result\":{%s}}\n",
		result_body ? result_body : "");
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

static void control_send_err(const char *msg)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

// Returns 1 if rx currently holds a complete newline-terminated line.
static int rx_has_line(void)
{
	for (int i = 0; i < rx_len; i++) {
		if (rx[i] == '\n') return 1;
	}
	return 0;
}

// Pops the first buffered line out of rx into out (without the newline).
// Only valid to call when rx_has_line() is true.
static void rx_take_line(char *out, int cap)
{
	for (int i = 0; i < rx_len; i++) {
		if (rx[i] == '\n') {
			int len = i < cap - 1 ? i : cap - 1;
			memcpy(out, rx, len);
			out[len] = 0;
			// strip trailing CR
			if (len && out[len-1] == '\r') out[len-1] = 0;
			memmove(rx, rx + i + 1, rx_len - i - 1);
			rx_len -= i + 1;
			return;
		}
	}
}

// Read one newline-terminated request line into out (without the newline).
// Returns 1 on success, 0 on disconnect.
static int control_read_line(char *out, int cap)
{
	for (;;) {
		if (rx_has_line()) {
			rx_take_line(out, cap);
			return 1;
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
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(hello)) n = (int)sizeof(hello) - 1;
	send(control_sock, hello, n, 0);
}

static unsigned long control_frame = 0;   // frames since boot
static int step_remaining = 0;            // >0 while a step is in progress
static int running = 0;                   // free-run mode

static void send_frame_result(void)
{
	char body[64];
	snprintf(body, sizeof(body), "\"frame\":%lu", control_frame);
	control_send_ok(body);
}

// Dispatch a single request line. Returns:
//   0 = handled inline (reply already sent), stay paused
//   1 = advance the emulator (step/run); reply is sent at the next boundary
//   (Tasks 9-10 extend this for pad/screenshot/reset)
int control_dispatch(char *line)
{
	if (!strcmp(line, "ping")) { control_send_ok("\"pong\":true"); return 0; }
	if (!strcmp(line, "version")) {
		char body[128];
		snprintf(body, sizeof(body), "\"version\":\"%s\",\"protocol\":1", BLASTEM_VERSION);
		control_send_ok(body);
		return 0;
	}
	if (!strncmp(line, "step ", 5)) {
		if (running) { control_send_err("cannot step while running; pause first"); return 0; }
		int n = atoi(line + 5);
		if (n <= 0) { control_send_err("step needs a positive frame count"); return 0; }
		step_remaining = n;
		return 1;  // break pause loop so frames advance; reply sent at boundary
	}
	if (!strcmp(line, "run")) {
		running = 1;
		control_send_ok("\"mode\":\"run\"");
		return 1;  // break pause loop; free-run
	}
	if (!strcmp(line, "pause")) {
		// only meaningful mid-run; handled in the boundary poll. If paused, just report.
		running = 0;
		send_frame_result();
		return 0;
	}
	// pad/screenshot/reset added in Tasks 9-10
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

// Called at every Genesis frame boundary while the control server is active.
// Drives the step/run/pause state machine.
void control_frame_boundary(void *sys, unsigned int elapsed)
{
	if (control_sock < 0) return;
	control_frame += elapsed;

	if (step_remaining > 0) {
		step_remaining -= (int)elapsed;
		if (step_remaining <= 0) {
			step_remaining = 0;
			send_frame_result();
			control_pause_loop(sys);   // block for the next command
		}
		return;
	}

	if (running) {
		// non-blocking drain of any pending commands (pad/pause/screenshot)
		control_poll(sys);
		return;
	}

	// paused and not stepping: block until told to advance
	control_pause_loop(sys);
}

static void set_nonblocking(int s, int on)
{
#ifdef _WIN32
	u_long m = on; ioctlsocket(s, FIONBIO, &m);
#else
	int fl = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

// Non-blocking drain of any commands the client sent while free-running.
void control_poll(void *sys)
{
	set_nonblocking(control_sock, 1);
	char line[2048];
	for (;;) {
		int got = recv(control_sock, rx + rx_len, (int)sizeof(rx) - rx_len, 0);
		if (got > 0) { rx_len += got; continue; }
		if (got == 0) {          // peer closed the connection
			CTL_CLOSE(control_sock);
			control_sock = -1;
			exit(0);
		}
		break;                    // got < 0: no data available right now
	}
	set_nonblocking(control_sock, 0);
	// dispatch any complete lines currently buffered
	while (rx_has_line()) {
		rx_take_line(line, sizeof(line));
		if (!strcmp(line, "pause")) { running = 0; send_frame_result(); control_pause_loop(sys); return; }
		control_dispatch(line);
	}
}
