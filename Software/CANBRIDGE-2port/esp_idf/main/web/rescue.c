#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/sockets.h"

/* Backtrace */
#include "esp_debug_helpers.h"

static const char *TAG = "RESCUE_SERVER";
static httpd_handle_t server = NULL;

/* Embedded Files */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t qrcode_js_start[]  asm("_binary_qrcode_min_js_start");
extern const uint8_t qrcode_js_end[]    asm("_binary_qrcode_min_js_end");

/*****************************************************************************
 * WEBSOCKET SERVER
 *****************************************************************************/
#include "ws_server.h"

struct ws_server ws_server;

/** WebSocket logger hook. Broadcasts Logs into Web */
int ws_logger_hook(const char *fmt, va_list tag) {
	int written;

	/* 1. Copy the va_list so we don't use a corrupted one */
	va_list args_copy;
	va_copy(args_copy, tag);

	// 2. Check a "recursion guard" using a static flag or Task Local Storage
	static bool inside_log = false;

	// 3. Standard print
	written = vprintf(fmt, tag);

	// 4. If we're not inside log - use custom printf
	if (!inside_log) {
		inside_log = true; // LOCK: Prevent re-entry

		char buf[128];

		written = vsnprintf(buf, sizeof(buf), fmt, args_copy);

		if ((strstr(buf, "ws_server")  == NULL) &&
		    (strstr(buf, "httpd_txrx") == NULL) &&
                    (strstr(buf, "httpd_ws")   == NULL)) {
			ws_server_broadcast_text(&ws_server, buf, written);
		}

		va_end(args_copy);

		inside_log = false; // UNLOCK
	}

	return written;
}

/*****************************************************************************
 * HTTP stuff
 *****************************************************************************/
// --- 3. HTTP Handlers ---
static esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
	// 1. Force the status to 302
	httpd_resp_set_status(req, "302 Found");
	
	// 2. Point it strictly to your IP. Do NOT use a domain name here.
	httpd_resp_set_hdr(req, "Location", "http://7.7.7.7/");
	
	// 3. IMPORTANT: Tell Windows NOT to cache this redirect.
	// If it caches it, it might try to go to 7.7.7.7 even when you are home!
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
	httpd_resp_set_hdr(req, "Pragma", "no-cache");
	httpd_resp_set_hdr(req, "Expires", "0");

	// 4. Send a tiny body. Some Windows versions ignore headers-only redirects.
	return httpd_resp_send(req, "Connect to Rescue Portal", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_get_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

// --- 4. Wi-Fi & System Init ---
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
	if (id == WIFI_EVENT_AP_STACONNECTED) {
		wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) data;
		ESP_LOGI(TAG, "Device " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
	}
}

static void init_services() {    
	// 1. Wi-Fi Config
	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
	esp_netif_ip_info_t ip_info;
	esp_netif_set_ip4_addr(&ip_info.ip, 7, 7, 7, 7);
	esp_netif_set_ip4_addr(&ip_info.gw, 7, 7, 7, 7);
	esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
	esp_netif_dhcps_stop(ap_netif);
	esp_netif_set_ip_info(ap_netif, &ip_info);
	esp_netif_dhcps_start(ap_netif);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	cfg.static_rx_buf_num = 16; // Stability boost
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

	wifi_config_t wifi_config = {
		.ap = { .ssid = "ESP32_RESCUE_PORTAL", .channel = 1, .password = "12345678", 
				.max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK }
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	esp_wifi_set_inactive_time(WIFI_IF_AP, 300);
}

static esp_err_t qrcode_js_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "application/javascript");
	return httpd_resp_send(req, (const char *)qrcode_js_start, qrcode_js_end - qrcode_js_start);
}

/** Must be called after websocket connection is closed
 *  can be passed as close_fn in server config */
void ws_client_del_thread_safe(httpd_handle_t hd, int fd)
{
	ws_server_client_del(&ws_server, fd);

	// 2. CRITICAL: Close the socket so the system can reuse it!
	// This is why Error 23 was happening.
	close(fd);
}

extern esp_err_t update_options_handler(httpd_req_t *req);
extern esp_err_t update_post_handler(httpd_req_t *req);

static void start_webserver(void) {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	//config.lru_purge_enable = true; 
	config.lru_purge_enable = false; 
	config.max_open_sockets = 5;
	config.stack_size = 8192; // More stack for WebSocket + Redirect logic
	
	config.keep_alive_enable   = true; /* Cleanup zombie sockets */
	config.keep_alive_idle     = 5;    // 5 seconds of silence before probing
	config.keep_alive_interval = 2;    // 2 seconds between probes
	config.keep_alive_count    = 3;    // 3 failed probes = close socket

	config.max_uri_handlers = 12;

	/* Register websocket stuff */
	config.close_fn = ws_client_del_thread_safe;

	ws_server_init(&ws_server);

	if (httpd_start(&server, &config) == ESP_OK) {
		static httpd_uri_t uris[7] = {
			{ .uri = "/", .method = HTTP_GET, .handler = index_get_handler },
			{ .uri = "/qrcode.min.js", .method = HTTP_GET, .handler = qrcode_js_handler },
			{ .uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler },
			{ .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler },
			{ .uri = "/redirect", .method = HTTP_GET, .handler = captive_portal_redirect_handler },
			{ .uri = "/update", .method = HTTP_OPTIONS, .handler = update_options_handler },
			{ .uri = "/update", .method = HTTP_POST, .handler = update_post_handler }
		};
		for (int i = 0; i < 7; i++) httpd_register_uri_handler(server, &uris[i]);

		//httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_portal_redirect_handler);
	}

	ws_server_start(&ws_server, &server);
}

// The function you want to run every second
void my_periodic_action() {
	char *text;
	if (ws_server_peek_text_rx(&ws_server, &text) != 0) {
		ESP_LOGI(TAG, "message arrived! %s", text);
		ws_server_dequeue_text_rx(&ws_server, text);
	}
}

void second_timer_task(void *pvParameters) {
	// TickType_t stores the time of the last wake-up
	TickType_t xLastWakeTime = xTaskGetTickCount();
	
	// Define the period (1000ms converted to FreeRTOS ticks)
	const TickType_t xFrequency = pdMS_TO_TICKS(1000);

	while (1) {
		// Wait for the next cycle
		vTaskDelayUntil(&xLastWakeTime, xFrequency);

		// Call your function
		my_periodic_action();
	}
}

void dns_server_task(void *pvParameters);

void rescue_main(void) {
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	init_services();

	start_webserver();

	// Launch background tasks
	xTaskCreate(dns_server_task,   "dns_server",   3072, NULL, 1, NULL);
	xTaskCreate(second_timer_task, "one_sec_timer", 2048, NULL, 5, NULL);

	// Finally, redirect system logs to our ring buffer
	esp_log_set_vprintf(ws_logger_hook);
	
	ESP_LOGI(TAG, "System Ready at http://7.7.7.7");
}
