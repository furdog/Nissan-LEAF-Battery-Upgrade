#include "ws_server.h"

static const char *TAG = "ws_server";

/* Prototypes */
static void ws_server_task(void *pv_parameters);

ESP_EVENT_DECLARE_BASE(WS_SERVER_EVENTS);
ESP_EVENT_DEFINE_BASE(WS_SERVER_EVENTS);

/******************************************************************************
 * CORE
 *****************************************************************************/
/** WS handler for httpd. TODO refactor */
static esp_err_t ws_server_handler(httpd_req_t *req)
{
	struct ws_server *self = (struct ws_server *)req->user_ctx;

	esp_err_t ret;

	/* 1. Check arguments */
	if ((self == NULL) || (req == NULL)) {
		ESP_LOGE(TAG, "Invalid argument/s");
		ret = ESP_ERR_INVALID_ARG;
		return ret;
	}

	/* 2. Register client */
	if (req->method == HTTP_GET) {
		int websocket_fd = httpd_req_to_sockfd(req);
		ws_server_client_add(self, websocket_fd);

		return ESP_OK;
	}

	/* 3. Receive packet */
	httpd_ws_frame_t ws_pkt;
	memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
	// If this returns anything other than ESP_OK, the server kills the session
	ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "WS recv failed, killing session");
		return ESP_FAIL; // <--- THIS is what triggers the close_fn
	}

	if (ws_pkt.len) {
		void *buf = NULL;
		if (xRingbufferSendAcquire(self->rb.text_rx, &buf, ws_pkt.len + 1u, 0u) == pdFALSE) {
			ESP_LOGE(TAG, "Failed to allocate memory in text_rx");
			return ESP_ERR_NO_MEM;
		}

		ws_pkt.payload = buf;
		/* Set max_len = ws_pkt.len to get the frame payload */
		ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
		((char *)buf)[ws_pkt.len] = '\0';

		xRingbufferSendComplete(self->rb.text_rx, buf);

		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
			return ret;
		}
		ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
	}

	return ESP_OK;
}

extern void ws_server_init(struct ws_server *self)
{
	size_t i; /* Common iterater */

	/* 1. Init ring buffers */
	self->rb.text_tx = NULL;

	/* 2. Init locks */
	self->lc.client_list = NULL;
	self->lc.text_tx     = NULL;
	self->lc.text_rx     = NULL;

	/* 3. Init httpd server handle */
	self->httpd_handle = NULL;

	/* 4. Init clients */
	for (i = 0u; i < WS_SERVER_MAX_CLIENTS; i++) {
		self->clients[i].fd = -1;
	}
	self->clients_count = 0u;

	/* 5. Init task related stuff */
	self->task_handle = NULL;
	self->is_running  = false;

	/* 6. Init httpd uri */
	memset(&self->ws_uri, 0, sizeof(httpd_uri_t));
	self->ws_uri.uri          = "/ws";
	self->ws_uri.method       = HTTP_GET;
	self->ws_uri.handler      = ws_server_handler;
	self->ws_uri.user_ctx     = self;
	self->ws_uri.is_websocket = true;
}

static void ws_server_free(struct ws_server *self)
{
	/* 1. Free all ring buffers */
	if (self->rb.text_tx != NULL) {
		vRingbufferDelete(self->rb.text_tx);
	}

	if (self->rb.text_rx != NULL) {
		vRingbufferDelete(self->rb.text_rx);
	}

	/* 2. Free all locks */
	if (self->lc.client_list != NULL) {
		vSemaphoreDelete(self->lc.client_list);
	}

	if (self->lc.text_tx != NULL) {
		vSemaphoreDelete(self->lc.text_tx);
	}
	
	if (self->lc.text_rx != NULL) {
		vSemaphoreDelete(self->lc.text_rx);
	}

	/* 3. Clean server state */
	ws_server_init(self);
}

extern void ws_server_stop(struct ws_server *self)
{
	if (self == NULL) return;

	ESP_LOGI(TAG, "stop()");

	httpd_unregister_uri_handler(self->httpd_handle, self->ws_uri.uri,
				     self->ws_uri.method);

	/* 1. Signal the task to stop */
	self->is_running = false;

	/* 2. Give the task time to exit its loop and delete itself */
	while (self->task_handle != NULL) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	// 3. Clean up memory/mutexes
	ws_server_free(self);
}

extern esp_err_t ws_server_start(struct ws_server *self,
				 httpd_handle_t *httpd_handle)
{
	esp_err_t err = ESP_OK;

	ESP_LOGI(TAG, "Starting...");

	/* 1. Check arguments */
	if ((self == NULL) || (httpd_handle == NULL)) {
		ESP_LOGE(TAG, "Invalid argument/s");
		err = ESP_ERR_INVALID_ARG;
	}

	/* 2. Assign httpd handle */
	self->httpd_handle = httpd_handle;

	/* 3. Create and check ring buffers */
	self->rb.text_tx = xRingbufferCreate(4096, RINGBUF_TYPE_NOSPLIT);
	self->rb.text_rx = xRingbufferCreate(4096, RINGBUF_TYPE_NOSPLIT);

	if (self->rb.text_tx == NULL) {
		ESP_LOGE(TAG, "Failed to create text_tx ring buffer!");
		err = ESP_FAIL;
	}

	if (self->rb.text_rx == NULL) {
		ESP_LOGE(TAG, "Failed to create text_rx ring buffer!");
		err = ESP_FAIL;
	}

	/* 4. Create and check locks buffers */
	self->lc.client_list = xSemaphoreCreateMutex();
	self->lc.text_tx     = xSemaphoreCreateMutex();
	self->lc.text_rx     = xSemaphoreCreateMutex();

	if (self->lc.client_list == NULL) {
		ESP_LOGE(TAG, "Failed to create client list lock!");
		err = ESP_FAIL;
	}

	if (self->lc.text_tx == NULL) {
		ESP_LOGE(TAG, "Failed to create text_tx lock!");
		err = ESP_FAIL;
	}

	if (self->lc.text_rx == NULL) {
		ESP_LOGE(TAG, "Failed to create text_rx lock!");
		err = ESP_FAIL;
	}

	/* 5. Create main server task */
	if (err == ESP_OK) {
		self->is_running = true;

		BaseType_t ret = xTaskCreate(
			ws_server_task,
			"ws_srv_task",
			4096,
			(void *)self,
			2,
			&self->task_handle
		);

		if (ret != pdPASS) {
			ESP_LOGE(TAG, "Failed to create main task");
			err = ESP_FAIL;
		}
	}

	/* 6. Set WS uri handler */
	if (err == ESP_OK) {
		err = httpd_register_uri_handler(*self->httpd_handle,
						 &self->ws_uri);

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to register URI handle");
		}
	}

	/* 7. Report success, otherwise free all resources */
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "Failure!");
		ws_server_stop(self);
	} else {
		ESP_LOGI(TAG, "Success!");
	}

	return err;
}

/******************************************************************************
 * CLIENTS
 *****************************************************************************/
static esp_err_t ws_server_client_try_insert(struct ws_server *self,
					     size_t slot, int fd)
{
	esp_err_t err = ESP_OK;

	err = esp_event_post(WS_SERVER_EVENTS,
			     WS_SERVER_EVENT_CLIENT_CONNECTED,
			     &fd, sizeof(fd),
			     pdMS_TO_TICKS(10));

	if (err == ESP_OK) {
		self->clients[slot].fd = fd;
		self->clients_count++;
		ESP_LOGI(TAG, "(add) New client fd: %d", fd);
	} else if (err == ESP_ERR_TIMEOUT) {
		ESP_LOGE(TAG, "(add) Event queue full, fd: %d", fd);
	} else {
		ESP_LOGE(TAG, "(add) event post failed: %s", esp_err_to_name(err));
	}

	return err;
}

extern esp_err_t ws_server_client_add(struct ws_server *self, int fd)
{
	esp_err_t err = ESP_OK;

	/* 1. Check arguments */
	if ((self == NULL) || (fd < 0)) {
		ESP_LOGE(TAG, "(add) Invalid argument/s");
		err = ESP_ERR_INVALID_ARG;

	/* 2. Lock client list */
	} else if (xSemaphoreTake(self->lc.client_list, pdMS_TO_TICKS(1000)) ==
		   pdTRUE) {
		size_t i;
		int free_slot = -1;
		bool already_exists = false;

		/* 3. Scan list for duplicates and find first free slot */
		for (i = 0u; i < WS_SERVER_MAX_CLIENTS; i++) {
			if (self->clients[i].fd == fd) {
				already_exists = true;
				break;
			}
			if (free_slot == -1 && self->clients[i].fd == -1) {
				free_slot = i;
			}
		}

		/* 4. Add new client if it's unique and space is available */
		if (already_exists) {
			ESP_LOGW(TAG, "(add) fd %d already in list", fd);
			err = ESP_ERR_INVALID_STATE;
		} else if (free_slot != -1) {
			err = ws_server_client_try_insert(self, free_slot, fd);
		} else {
			ESP_LOGE(TAG, "(add) No free slots for fd %d", fd);
			err = ESP_ERR_NO_MEM;
		}

		xSemaphoreGive(self->lc.client_list);
	} else {
		/* 3. This basically should never happen. */

		ESP_LOGE(TAG, "(add) lock timeout");
		err = ESP_ERR_TIMEOUT;
	}

	return err;
}

static esp_err_t ws_server_client_try_remove(struct ws_server *self,
					     size_t slot, int fd)
{
	esp_err_t err = ESP_OK;

	/* 1. Register server event */
	err = esp_event_post(WS_SERVER_EVENTS,
			     WS_SERVER_EVENT_CLIENT_DISCONNECTED,
			     &fd, sizeof(fd),
			     pdMS_TO_TICKS(50));

	/* 2. Drop fd in any case */
	self->clients[slot].fd = -1;
	self->clients_count--;

	if (err == ESP_ERR_TIMEOUT) {
		ESP_LOGE(TAG, "(del) Event queue full, fd: %d", fd);
	} else if (err != ESP_OK) {
		ESP_LOGE(TAG, "(del) Event post failed: %s", esp_err_to_name(err));
	}

	ESP_LOGI(TAG, "(del) Removed client fd: %d", fd);

	return err;
}

extern esp_err_t ws_server_client_del(struct ws_server *self, int fd)
{
	esp_err_t err = ESP_OK;

	/* 1. Check arguments */
	if ((self == NULL) || (fd < 0)) {
		ESP_LOGE(TAG, "(del) Invalid argument/s");
		err = ESP_ERR_INVALID_ARG;

	/* 2. Lock client list */
	} else if (xSemaphoreTake(self->lc.client_list, pdMS_TO_TICKS(1000)) 
	           == pdTRUE) {
		size_t i;
		bool found = false;

		/* 3. Remove client from list */
		for (i = 0u; i < WS_SERVER_MAX_CLIENTS; i++) {
			if (self->clients[i].fd == fd) {
				err = ws_server_client_try_remove(self, i, fd);

				found = true;

				break;
			}
		}

		xSemaphoreGive(self->lc.client_list);

		if (!found) {
			ESP_LOGW(TAG, "(del) fd %d not found in list", fd);
			err = ESP_ERR_NOT_FOUND;
		}

	} else {
		/* 3. This basically should never happen. */
		ESP_LOGE(TAG, "(del) lock timeout");
		err = ESP_ERR_TIMEOUT;
	}

	return err;
}

/** Queue RX message, no client context assumed (generic message) */
extern esp_err_t ws_server_queue_text_rx(struct ws_server *self,
					 const char *text, size_t len)
{
	esp_err_t err = ESP_OK;

	/* 1. Check arguments */
	if ((self == NULL) || (text == NULL) || (len == 0)) {
		ESP_LOGE(TAG, "Invalid argument/s");
		err = ESP_ERR_INVALID_ARG;
	}

	/* 2. Put text into a ring buffer */
	if(xRingbufferSend(self->rb.text_rx, text, len, 0) != pdTRUE) {
		//ESP_LOGE(TAG, "Broadcast ring buffer is full!");
		err = ESP_FAIL;
	}

	return err;
}

/******************************************************************************
 * MESSAGES
 *****************************************************************************/
/** Broadcasts text message */
esp_err_t _ws_server_broadcast_text(struct ws_server *self,
				   httpd_ws_frame_t *frame)
{
	struct ws_server_client clients_snapshot[WS_SERVER_MAX_CLIENTS];

	if (xSemaphoreTake(self->lc.client_list, 1000) ==
	    pdTRUE) {
		memcpy(clients_snapshot, self->clients,
		       sizeof(clients_snapshot));

		xSemaphoreGive(self->lc.client_list);

		for (int i = 0; i < WS_SERVER_MAX_CLIENTS; i++) {
			if (clients_snapshot[i].fd != -1) {
				//esp_err_t err = httpd_ws_send_frame_async(*self->httpd_handle,
				//			  clients_snapshot[i].fd,
				//			  frame);
				//ESP_LOGI(TAG, "(0) fd: %i, handle: %p, frame.len: %i",
				//	      clients_snapshot[i].fd, *self->httpd_handle, frame->len);

				esp_err_t err = httpd_ws_send_data(
						  *self->httpd_handle,
						   clients_snapshot[i].fd,
						   frame);

				//ESP_LOGI(TAG, "httpd_ws_send_data sending data to fd: %i", clients_snapshot[i].fd);
				if (err != ESP_OK) {
					ESP_LOGE(TAG, "httpd_ws_send_data failed fd: %i (%s)", clients_snapshot[i].fd, esp_err_to_name(err));
				}
			}
		}
	} else {
		ESP_LOGE(TAG, "_ws_server_broadcast_text, failed to acquire lock");
	}

	return ESP_OK;
}

/** Schedules text message reception */
static void ws_schedule_text_message(struct ws_server *self)
{
	size_t item_size;
	char *item = (char *)xRingbufferReceive(self->rb.text_tx,
					&item_size, 0);
        if (item) {
		httpd_ws_frame_t frame = {
			.payload = (uint8_t *)item,
			.len = item_size,
			.type = HTTPD_WS_TYPE_TEXT
		};

		_ws_server_broadcast_text(self, &frame);

		/* Free space inside ring buffer. */
		vRingbufferReturnItem(self->rb.text_tx, (void *)item);
	}
}

/** Main message scheduler task */
static void ws_server_task(void *pv_parameters)
{
	struct ws_server *self = (struct ws_server *)pv_parameters;

	while (self->is_running) {
		/* We only schedule text messages atm */
		ws_schedule_text_message(self);

		vTaskDelay(pdMS_TO_TICKS(10));
	}

	self->task_handle = NULL; 
	vTaskDelete(NULL);
}

/** Broadcast text message via websockets */
extern esp_err_t ws_server_broadcast_text(struct ws_server *self,
					  const char *text, size_t len)
{
	esp_err_t err = ESP_OK;

	/* 1. Check arguments TODO check ws_server state */
	if ((self == NULL) || (text == NULL) ||
	    (len == 0)) {
		ESP_LOGE(TAG, "Invalid argument/s");
		err = ESP_ERR_INVALID_ARG;
	}

	/* 2. Put text into a ring buffer */
	if(xRingbufferSend(self->rb.text_tx, text, len, 0) != pdTRUE) {
		ESP_LOGE(TAG, "Broadcast ring buffer is full!");
		err = ESP_FAIL;
	}

	return err;
}

/** Peek RX message, no client context assumed (generic message)
 *  Must be dequeued later */
extern size_t ws_server_peek_text_rx(struct ws_server *self, char **text)
{
	 size_t item_size;
	*text = (char *)xRingbufferReceive(self->rb.text_rx,
					  &item_size, 0);

	return item_size;
}

/** Dequeue RX message, no client context assumed (generic message) */
extern void ws_server_dequeue_text_rx(struct ws_server *self, char *text)
{
	if (text) {
		/* Free space inside ring buffer. */
		vRingbufferReturnItem(self->rb.text_rx, (void *)text);
	}
}
