#ifndef CONTROL_H_
#define CONTROL_H_

extern int control_enabled;

// Bind/listen on 127.0.0.1:port, accept one client, send the hello event.
void control_init(int port);
int control_active(void);
// Send a raw JSON event line, e.g. control_send_event("\"event\":\"kdebug\",\"message\":\"X\"")
void control_send_event(const char *json_body);
int control_dispatch(char *line);
void control_pause_loop(void *sys);
void control_frame_boundary(void *sys, unsigned int elapsed_frames);
void control_poll(void *sys);
// Called when the 68000 acknowledges VInt (level 6): counts it and latches the
// sound chip state for "sound_state vint".
void control_vint_ack(void *sys);
unsigned long control_vint_count(void);

#endif
