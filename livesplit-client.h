#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	LIVESPLIT_PHASE_NOT_RUNNING = 0,
	LIVESPLIT_PHASE_RUNNING,
	LIVESPLIT_PHASE_ENDED,
	LIVESPLIT_PHASE_PAUSED,
	LIVESPLIT_PHASE_DISCONNECTED
} livesplit_phase_t;

typedef struct {
	livesplit_phase_t phase;
	char game[256];
	char category[256];
	int attempt_count;
} livesplit_state_t;

typedef void (*livesplit_phase_changed_cb)(void *param, livesplit_phase_t old_phase, livesplit_phase_t new_phase, const livesplit_state_t *state);

typedef struct livesplit_client livesplit_client_t;

livesplit_client_t *livesplit_client_create(const char *host, int port, livesplit_phase_changed_cb cb, void *param);
void livesplit_client_destroy(livesplit_client_t *client);
void livesplit_client_get_state(livesplit_client_t *client, livesplit_state_t *out_state);

#ifdef __cplusplus
}
#endif
