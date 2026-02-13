#include "esp_ota_ops.h"
#include "esp_http_server.h"

esp_err_t update_options_handler(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}


#define OTA_BUF_SIZE 4096

esp_err_t update_post_handler(httpd_req_t *req) {
	// Add CORS header to satisfy the browser preflight
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	esp_ota_handle_t update_handle = 0;
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	
	// Allocate a larger buffer to reduce TCP overhead
	char *ota_write_data = malloc(OTA_BUF_SIZE);
	if (!ota_write_data) {
		return ESP_ERR_NO_MEM;
	}

	int remaining = req->content_len;
	int ret;
	bool ota_started = false;

	while (remaining > 0) {
		// Read from socket into our 4KB buffer
		if ((ret = httpd_req_recv(req, ota_write_data, MIN(remaining, OTA_BUF_SIZE))) <= 0) {
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
			goto cleanup;
		}

		char *data_ptr = ota_write_data;
		int data_len = ret;

		// 1. Skip HTTP Multipart headers by hunting for the 0xE9 Magic Byte
		if (!ota_started) {
			for (int i = 0; i < ret; i++) {
				if ((uint8_t)ota_write_data[i] == 0xE9) {
					data_ptr = &ota_write_data[i];
					data_len = ret - i;
					
					// Initialize OTA only when valid binary data is found
					esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
					if (err != ESP_OK) goto cleanup;
					
					ota_started = true;
					break;
				}
			}
		}

		// 2. Write the chunk to Flash
		if (ota_started) {
			if (esp_ota_write(update_handle, data_ptr, data_len) != ESP_OK) {
				esp_ota_abort(update_handle);
				goto cleanup;
			}
		}

		remaining -= ret;
	}

	// 3. Finalize and Validate
	if (esp_ota_end(update_handle) != ESP_OK || 
		esp_ota_set_boot_partition(update_partition) != ESP_OK) {
		goto cleanup;
	}

	free(ota_write_data);
	httpd_resp_sendstr(req, "Update Success. Rebooting...");
	vTaskDelay(pdMS_TO_TICKS(1000));
	esp_restart();
	return ESP_OK;

cleanup:
	free(ota_write_data);
	httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Failed");
	return ESP_FAIL;
}
