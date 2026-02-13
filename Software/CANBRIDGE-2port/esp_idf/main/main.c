#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/twai.h"

#include "esp_sleep.h"

#include "esp_timer.h"

#include "can-bridge-firmware.h"

static const char *TAG = "can_bridge_main";

/******************************************************************************
 * SIMPLE TWAI ADAPTER FOR VARIOUS CAN RELATED PROJECTS (ESP32C6)
 * Preconfigured, default TWAI 500kbps, no filtering.
 *****************************************************************************/
struct simple_twai
{
	twai_handle_t bus;
	uint8_t id;

	gpio_num_t tx;
	gpio_num_t rx;
};

esp_err_t simple_twai_init(struct simple_twai *self)
{
	esp_err_t err = ESP_OK;

	twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
		self->tx, self->rx, TWAI_MODE_NORMAL);
	twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
	twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

	g_config.controller_id = self->id;

	if (err == ESP_OK) {
		err = twai_driver_install_v2(&g_config, &t_config, &f_config,
						 &self->bus);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "install fault");
		}
	}

	if (err == ESP_OK) {
		err = twai_start_v2(self->bus);

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "start fault");
		}
	}

	if (err == ESP_OK) {
		err = twai_reconfigure_alerts_v2(self->bus, TWAI_ALERT_BUS_OFF,
						 NULL);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "reconfigure alerts fault");
		}
	}

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "err:%s in %s",  esp_err_to_name(err), __func__);
	}

	return err;
}

esp_err_t simple_twai_kill(struct simple_twai *self)
{
	esp_err_t err = ESP_OK;

	err = twai_driver_uninstall_v2(self->bus);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "err:%s in %s",  esp_err_to_name(err), __func__);
	}

	return err;
}

esp_err_t simple_twai_send(struct simple_twai *self,
			   twai_message_t *msg)
{
	esp_err_t err = ESP_OK;

	err = twai_transmit_v2(self->bus, msg, 0);

	if (err != ESP_OK) {
		ESP_LOGD(TAG, "err:%s in %s",  esp_err_to_name(err), __func__);
	}

	return err;
}

esp_err_t simple_twai_recv(struct simple_twai *self,
			   twai_message_t *msg)
{
	esp_err_t err = ESP_OK;

	err = twai_receive_v2(self->bus, msg, 0);

	if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
		ESP_LOGD(TAG, "err:%s in %s",  esp_err_to_name(err), __func__);
	}

	return err;
}

esp_err_t simple_twai_update(struct simple_twai *self)
{
	esp_err_t err = ESP_OK;
	uint32_t alerts = 0;

	err = twai_read_alerts_v2(self->bus, &alerts, 0);

	if (err == ESP_OK && (alerts & TWAI_ALERT_BUS_OFF)) {
		ESP_LOGW(TAG, "bus off alert (%u)", self->id);

		/* Reset TWAI in case of bus off */
		err = twai_reconfigure_alerts_v2(self->bus, 0, NULL);

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "reconfigure alerts fault");
		}

		/* TODO also check for errors. */
		simple_twai_kill(self);
		simple_twai_init(self);
	}

	if (err != ESP_OK) {
		ESP_LOGD(TAG, "err:%s in %s",  esp_err_to_name(err), __func__);
	}

	return err;
}

/******************************************************************************
 * CAN BRIDGE (STM32 to ESP-IDF adapter)
 *****************************************************************************/
#include "driver/twai.h"

// Array to map canNum (0 or 1) to your bus handles
extern struct simple_twai stw0;
extern struct simple_twai stw1;
struct simple_twai *twai_channels[2] = { &stw0, &stw1 };

/** Helper: Converts CAN_FRAME to ESP32 twai_message_t */
void map_to_twai(CAN_FRAME *src, twai_message_t *dest) {
	dest->identifier = src->ID;
	dest->data_length_code = src->dlc;
	dest->extd = 0; // 0 for standard 11-bit, 1 for extended 29-bit
	dest->rtr = 0;
	for(int i = 0; i < src->dlc; i++) {
		dest->data[i] = src->data[i];
	}
}

/** Helper: Converts ESP32 twai_message_t back to CAN_FRAME */
void map_from_twai(twai_message_t *src, CAN_FRAME *dest) {
	dest->ID = src->identifier;
	dest->dlc = src->data_length_code;
	for(int i = 0; i < src->data_length_code; i++) {
		dest->data[i] = src->data[i];
	}
}

/** It sends can to TX
 *  WARNING: TxRx param is ignored */
CQ_STATUS PushCan( uint8_t canNum, uint8_t TxRx, CAN_FRAME *frame )
{
	if( canNum > 1 ) return CQ_IGNORED;

	twai_message_t msg;
	map_to_twai(frame, &msg);

	// We call your existing simple_twai_send wrapper
	esp_err_t err = simple_twai_send(twai_channels[canNum], &msg);

	return (err == ESP_OK) ? CQ_OK : CQ_FULL;
}

/** It receives can from RX
 *  WARNING: TxRx param is ignored */
CQ_STATUS PopCan( uint8_t canNum, uint8_t TxRx, CAN_FRAME *frame )
{
	if( canNum > 1 ) return CQ_IGNORED;

	twai_message_t msg;
	// We call your existing simple_twai_recv wrapper
	esp_err_t err = simple_twai_recv(twai_channels[canNum], &msg);

	if (err == ESP_OK) {
		map_from_twai(&msg, frame);
		return CQ_OK;
	}

	return CQ_EMPTY;
}

/** Gets queued messages length
 *  WARNING: TxRx param is ignored */
uint8_t LenCan( uint8_t canNum, uint8_t TxRx )
{
	if( canNum > 1 ) return 0;

	twai_status_info_t status;
	twai_get_status_info_v2(twai_channels[canNum]->bus, &status);

	// Returns the number of messages waiting in the RX or TX queue
	return (TxRx == 0) ? status.msgs_to_tx : status.msgs_to_rx;
}

void can_bridge_light_sleep()
{
	esp_sleep_enable_gpio_wakeup();
	gpio_wakeup_enable(stw0.rx, GPIO_INTR_LOW_LEVEL);
	gpio_wakeup_enable(stw1.rx, GPIO_INTR_LOW_LEVEL);

	ESP_LOGI(TAG, "Entering sleep. Waiting for CAN traffic...");
	esp_light_sleep_start();
	ESP_LOGI(TAG, "Wakeup detected on RX pin!");
}

void can_bridge_main_loop() {
	static CAN_FRAME frame;

	static uint8_t  idle_seconds = 0u;
	static uint32_t timer_ms     = 0u;

	/* Delta time calculation stuff */
	static uint32_t delta_timestamp_ms = 0u;
		   uint32_t timestamp_ms       = esp_timer_get_time() / 1000u;
	uint32_t delta_time_ms = timestamp_ms - delta_timestamp_ms;
	delta_timestamp_ms = timestamp_ms;

	timer_ms += delta_time_ms;

	if (timer_ms >= 1000u) {
		timer_ms -= 1000u;

		one_second_ping(); /* can-bridge-firmware.c */

		if((LenCan( MYCAN1, CAN_RX )) == 0 && (LenCan( MYCAN2, CAN_RX ) == 0)){
			//Can bus is idle
			idle_seconds++;

			if(idle_seconds > 5) { //No can messages for 5s
				//can_bridge_light_sleep();

				idle_seconds = 0;
			}
		}
		}

		if( LenCan( MYCAN1, CAN_RX ) > 0 ) {
		idle_seconds = 0;
		PopCan( MYCAN1, CAN_RX, &frame );

		can_handler( MYCAN1, &frame );
	}

		if( LenCan( MYCAN2, CAN_RX ) > 0 ) {
		idle_seconds = 0;
		PopCan( MYCAN2, CAN_RX, &frame );
		can_handler( MYCAN2, &frame );
	}
}

/******************************************************************************
 * MAIN
 *****************************************************************************/
struct simple_twai stw0;
struct simple_twai stw1;

void rescue_main(void);

void app_main(void)
{
	rescue_main(); /* Start RESCUE SERVER */

	stw0.id = 0;
	stw0.tx = GPIO_NUM_14;
	stw0.rx = GPIO_NUM_15;
	simple_twai_init(&stw0);

	stw1.id = 1;
	stw1.tx = GPIO_NUM_18;
	stw1.rx = GPIO_NUM_19;
	simple_twai_init(&stw1);

	while (1) {
		simple_twai_update(&stw0);
		simple_twai_update(&stw1);

		can_bridge_main_loop();

		vTaskDelay(1);
	}
}
