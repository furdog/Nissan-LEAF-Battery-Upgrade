#include "freertos/ringbuf.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_event.h"

/******************************************************************************
 * RING BUFFERS
 *****************************************************************************/
struct ws_server_ring_buffers {
	RingbufHandle_t text;
};

/******************************************************************************
 * THREAD LOCKS
 *****************************************************************************/
struct ws_server_locks {
	SemaphoreHandle_t client_list;
	SemaphoreHandle_t text;
};

/******************************************************************************
 * CLIENT
 *****************************************************************************/
#define WS_SERVER_MAX_CLIENTS CONFIG_LWIP_MAX_SOCKETS

struct ws_server_client {
	int fd; /* BSD socket file descriptor */
};

/******************************************************************************
 * MAIN INSTANCE
 *****************************************************************************/
enum ws_server_event {
	WS_SERVER_EVENT_NONE,
	WS_SERVER_EVENT_CLIENT_CONNECTED,
	WS_SERVER_EVENT_CLIENT_DISCONNECTED,
};

struct ws_server {
	struct ws_server_ring_buffers rb;
	struct ws_server_locks        lc;

	httpd_handle_t *httpd_handle;

	/* Must be locked */
	struct ws_server_client clients[WS_SERVER_MAX_CLIENTS];
	size_t 			clients_count;

	/* Not neccessary to lock, but be careful */
	TaskHandle_t  task_handle;
	volatile bool is_running;
};

extern void ws_server_init(struct ws_server *self);
extern esp_err_t ws_server_start(struct ws_server *self,
				 httpd_handle_t *httpd_handle);
extern void      ws_server_stop (struct ws_server *self);

/******************************************************************************
 * CLIENTS
 *****************************************************************************/
extern esp_err_t ws_server_client_add(struct ws_server *self, int fd);
extern esp_err_t ws_server_client_del(struct ws_server *self, int fd);
extern esp_err_t ws_server_broadcast_text(struct ws_server *self,
					  const char *text, size_t len);
