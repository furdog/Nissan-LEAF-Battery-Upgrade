#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "dns_tools.h"

/*****************************************************************************
 * DNS
 *****************************************************************************/
static const char *TAG = "dns_server";

/** This task redirects all incoming DNS requests to 7.7.7.7 */
void dns_server_task(void *pvParameters) {
	uint8_t data[256]; // Increased size for safety

	struct sockaddr_in dest_addr = {
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_family = AF_INET,
		.sin_port = htons(53)
	};

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

	ESP_LOGI(TAG, "sniffer Active.");

	while (1) {
		struct sockaddr_in source_addr;
		socklen_t addr_len = sizeof(source_addr);
		int len = recvfrom(sock, data, sizeof(data), 0,
				   (struct sockaddr *)&source_addr, &addr_len);

		struct dns_msg msg;
		dns_msg_init(&msg, data, sizeof(data));
		dns_msg_parse_query(&msg, len);

		uint8_t answer[] = {
			0xc0, 0x0c,             // Pointer to name
			0x00, 0x01,             // Type A (Force IPv4)
			0x00, 0x01,             // Class IN
			0x00, 0x00, 0x00, 0x0a, // TTL 10s
			0x00, 0x04,             // Data Len 4
			7, 7, 7, 7              // IP 7.7.7.7
		};

		/* This answer redirects every request to 7.7.7.7 */
		size_t total_len = dns_msg_add_answer(&msg, answer,
							  sizeof(answer));

		sendto(sock, data, total_len, 0,
			   (struct sockaddr *)&source_addr, addr_len);

		if (msg.malformed == 0u) {
			uint32_t ip = source_addr.sin_addr.s_addr;

			ESP_LOGI(TAG, "Query from %u.%u.%u.%u "
				 "| Type: %s | Domain: %s",
				 (unsigned int)(ip & 0xFF),
				 (unsigned int)((ip >> 8) & 0xFF),
				 (unsigned int)((ip >> 16) & 0xFF),
				 (unsigned int)((ip >> 24) & 0xFF),
				 dns_msg_get_type_str(&msg), msg.name);
		} else {
			ESP_LOGI(TAG, "Query is malformed dns_tools.h, line: %u",
				 msg.malformed);
		}
	}
}
