#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/twai.h"

#include "esp_sleep.h"

#include "esp_timer.h"

#include "can-bridge-firmware.h"

#include "esp_task_wdt.h"

#include "sequencer/sequencer.h"

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

	/* For statistics */
	uint32_t tx_counter;
	uint32_t rx_counter;
};

esp_err_t simple_twai_init(struct simple_twai *self)
{
	esp_err_t err = ESP_OK;

	twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
		self->tx, self->rx, TWAI_MODE_NORMAL);
	twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
	twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

	g_config.rx_queue_len = 64;
	g_config.tx_queue_len = 64;

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

	self->tx_counter = 0u;
	self->rx_counter = 0u;

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

	if (err == ESP_OK) {
		self->tx_counter++;
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

	if (err == ESP_OK) {
		self->rx_counter++;
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
 * LED SEQUENCER
 *****************************************************************************/
#include "led_strip.h"

#define RGB_LED_PIN 8
led_strip_handle_t led_strip;

void led_strip_init() {
	led_strip_config_t strip_config = {
		.strip_gpio_num = RGB_LED_PIN,
		.max_leds = 1,
	};

	led_strip_rmt_config_t rmt_config = {
		.resolution_hz = 10 * 1000 * 1000, 
	};

	led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
}

#define LED_SEQ_MAX_ENTRIES 16u

enum led_seq_event {
	LED_SEQ_EVENT_NONE,

	LED_SEQ_EVENT_BLANK,
	LED_SEQ_EVENT_RED,
	LED_SEQ_EVENT_GREEN
};

struct sequencer       led_seq;
struct sequencer_entry led_seq_entries[LED_SEQ_MAX_ENTRIES];

void led_seq_blink_red(uint16_t post_delay_ms)
{
	/* Blink red for 250ms */
	sequencer_add_entry(&led_seq, 0,  LED_SEQ_EVENT_RED);
	sequencer_add_entry(&led_seq, 50, LED_SEQ_EVENT_BLANK);

	sequencer_add_entry(&led_seq, post_delay_ms, LED_SEQ_EVENT_BLANK);
}

void led_seq_init() {
	led_strip_init();
	sequencer_init(&led_seq, led_seq_entries, LED_SEQ_MAX_ENTRIES);
}

extern struct simple_twai stw0;
extern struct simple_twai stw1;

void led_seq_update(uint32_t delta_time_ms)
{
	uint8_t ev = 0u;
	static bool has_fault = true; /* Fault by default */

	/** Fill sequencer only in case if it's empty */
	if (sequencer_get_entry_count(&led_seq) == 0u) {
		if (stw0.rx_counter == 0u) {
			led_seq_blink_red(750);

			has_fault = true;
		}

		if (stw1.rx_counter == 0u) {
			led_seq_blink_red(200);
			led_seq_blink_red(750);

			has_fault = true;
		}

		/** If no elements was added, it means no errors */
		if ((sequencer_get_entry_count(&led_seq) == 0u) && has_fault) {
			sequencer_add_entry(&led_seq, 0, LED_SEQ_EVENT_GREEN);
			has_fault = false;
		}
	}

	ev = sequencer_update(&led_seq, delta_time_ms);

	switch (ev) {
	case LED_SEQ_EVENT_BLANK:
		//ESP_LOGW("LED", "BLANK");
		led_strip_clear(led_strip);
		led_strip_refresh(led_strip);
		break;
	
	case LED_SEQ_EVENT_RED:
		//ESP_LOGW("LED", "RED");
		led_strip_set_pixel(led_strip, 0, 20, 0, 0); 
		led_strip_refresh(led_strip);
		break;

	case LED_SEQ_EVENT_GREEN:
		//ESP_LOGW("LED", "GREEN");
		led_strip_set_pixel(led_strip, 0, 0, 20, 0); 
		led_strip_refresh(led_strip);
		break;

	case LED_SEQ_EVENT_NONE: default: break;
	}
}

/******************************************************************************
 * CUSTOM CAN HANDLER and WEB (for extra features)
 *****************************************************************************/
bool clim_ctl_recirc    = false;
bool clim_ctl_fresh_air = false;
bool clim_ctl_btn_alert = false;

void custom_can_handler(uint8_t can_bus, CAN_FRAME *frame)
{
	switch(frame->ID) {
	case 0x54B:
		if (frame->data[3] == 9u) {
			clim_ctl_recirc    = true;
			clim_ctl_fresh_air = false;
		} else if (frame->data[3] == (9u << 1u)) {
			clim_ctl_recirc    = false;
			clim_ctl_fresh_air = true;
		} else {}

		clim_ctl_btn_alert = ((frame->data[7] & 0x01u) > 0u) ?
						1u : 0u;
	}
}

void rescue_start(void);
void rescue_stop();

void check_softreset_sequence(uint32_t delta_time_ms)
{
	/* Stuff to reset cpu via leaf interface
	 * Will also reset wifi */
	static bool reset_trigger = false;
	static uint32_t reset_trigger_counter = 0u;
	static uint32_t reset_trigger_timer = 0u;

	static bool web_reset = false;
	//static bool cpu_reset = false;

	/* Count climate control button presses */
	if (clim_ctl_btn_alert != reset_trigger) {
		/* Count CC buttons presses */
		reset_trigger = clim_ctl_btn_alert;
		reset_trigger_counter++;
		reset_trigger_timer = 0u;
	}

	/* If no CC buttons been pressed in past second(-s) */
	if (reset_trigger_timer < 500u) {
		reset_trigger_timer += delta_time_ms;
	} else {
		/* Reset counter */
		web_reset = false;
		reset_trigger_counter = 0u;
	}

	/* If CC buttons was pressed 10 times in past second(-s) */
	if (reset_trigger_counter >= 10u && !web_reset) {
		web_reset = true;

		rescue_stop();
		rescue_start();
	}

	if (reset_trigger_counter >= 20u) {
		esp_restart();
	}
}

/******************************************************************************
 * CAN BRIDGE (STM32 to ESP-IDF adapter)
 *****************************************************************************/
// Array to map canNum (0 or 1) to your bus handles
extern struct simple_twai stw0;
extern struct simple_twai stw1;
struct simple_twai *twai_channels[2] = { &stw0, &stw1 };

uint32_t sent_tx = 0u;
uint32_t recv_rx = 0u;

/** Helper: Converts CAN_FRAME to ESP32 twai_message_t */
void map_to_twai(CAN_FRAME *src, twai_message_t *dest) {
	dest->identifier = src->ID;
	dest->data_length_code = src->dlc;
	dest->extd = 0; // 0 for standard 11-bit, 1 for extended 29-bit
	dest->rtr = 0;

	/* May cause bus off in case of many lost messages
	 * Set to 1 to enable single shot mode
	 * (vehicle may lose frames occasionally) */
	dest->ss  = 0;

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

	twai_message_t msg = {0};

	map_to_twai(frame, &msg);

	// We call your existing simple_twai_send wrapper
	esp_err_t err = simple_twai_send(twai_channels[canNum], &msg);

	if (err == ESP_OK) {
		sent_tx += 1;
	}

	return (err == ESP_OK) ? CQ_OK : CQ_FULL;
}

/** It receives can from RX
 *  WARNING: TxRx param is ignored */
CQ_STATUS PopCan( uint8_t canNum, uint8_t TxRx, CAN_FRAME *frame )
{
	if( canNum > 1 ) return CQ_IGNORED;

	twai_message_t msg = {0};

	// We call your existing simple_twai_recv wrapper
	esp_err_t err = simple_twai_recv(twai_channels[canNum], &msg);

	if (err == ESP_OK) {
		recv_rx += 1;
		map_from_twai(&msg, frame);
		return CQ_OK;
	}

	return CQ_EMPTY;
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

void log_twai_bus_status(twai_handle_t bus_a, twai_handle_t bus_b) {
	twai_status_info_t status_a;
	twai_status_info_t status_b;

	// Retrieve status for both handles
	if (twai_get_status_info_v2(bus_a, &status_a) != ESP_OK ||
		twai_get_status_info_v2(bus_b, &status_b) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get status info");
		return;
	}

	ESP_LOGE(TAG, "--- TWAI Bus Diagnostics ---");
	ESP_LOGI("","%-20s | %-12s | %-12s", "Metric", "Bus A", "Bus B");
	ESP_LOGI("","---------------------|--------------|--------------");
	
	// State (Running, Stopped, Bus-Off)
	ESP_LOGI("","%-20s | %-12d | %-12d", "State", status_a.state, status_b.state);
	
	// Messages waiting in the software ring buffer
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "Msgs Queued (RX)", status_a.msgs_to_rx, status_b.msgs_to_rx);
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "Msgs Queued (TX)", status_a.msgs_to_tx, status_b.msgs_to_tx);
	
	// Critical: How many messages were lost because the buffer was full
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "RX Overruns", status_a.rx_overrun_count, status_b.rx_overrun_count);
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "RX Missed"  , status_a.rx_missed_count,  status_b.rx_missed_count);
	
	// Hardware Error Counters
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "REC (RX Error)", status_a.rx_error_counter, status_b.rx_error_counter);
	ESP_LOGI("","%-20s | %-12lu | %-12lu", "TEC (TX Error)", status_a.tx_error_counter, status_b.tx_error_counter);
	
	ESP_LOGI("","---------------------------------------------------");
}

uint32_t cycle_counter = 0;
void can_bridge_main_loop() {
	static CAN_FRAME frame;

	static uint8_t  idle_seconds = 0u;
	static uint32_t timer_ms     = 0u;
	static uint32_t timer2_ms    = 0u;

	/* Delta time calculation stuff */
	static uint32_t delta_timestamp_ms = 0u;
		   uint32_t timestamp_ms       = esp_timer_get_time() / 1000u;
	uint32_t delta_time_ms = timestamp_ms - delta_timestamp_ms;
	delta_timestamp_ms = timestamp_ms;

	timer_ms += delta_time_ms;

	if (timer_ms >= 1000u) {
		timer_ms -= 1000u;

		one_second_ping(); /* can-bridge-firmware.c */

		//Can bus is idle
		idle_seconds++;

		if(idle_seconds > 5) { //No can messages for 5s
			//can_bridge_light_sleep();

			idle_seconds = 0;
		}
	}

	timer2_ms += delta_time_ms;
	cycle_counter++;

	if (timer2_ms >= 5000u) {
		timer2_ms -= 5000u;

		log_twai_bus_status(stw0.bus, stw1.bus);
		ESP_LOGI(TAG, "My_Leaf:    %u", My_Leaf);
		ESP_LOGI(TAG, "My_Battery: %u", My_Battery);
		ESP_LOGI(TAG, "sent_tx:    %u", sent_tx);
		ESP_LOGI(TAG, "recv_rx:    %u", recv_rx);
		ESP_LOGI(TAG, "cycle_cnt:  %u", cycle_counter);
		ESP_LOGI(TAG, "stw0 rx:%u tx:%u", stw0.rx_counter, stw0.tx_counter);
		ESP_LOGI(TAG, "stw1 rx:%u tx:%u", stw1.rx_counter, stw1.tx_counter);

		stw0.rx_counter = 0u;
		stw0.tx_counter = 0u;

		stw1.rx_counter = 0u;
		stw1.tx_counter = 0u;

		cycle_counter = 0;
	}

	if (PopCan( MYCAN1, CAN_RX, &frame ) == CQ_OK) {
		idle_seconds = 0;
		custom_can_handler( MYCAN1, &frame );
		can_handler( MYCAN1, &frame );
	}

	if (PopCan( MYCAN2, CAN_RX, &frame ) == CQ_OK) {
		idle_seconds = 0;
		custom_can_handler( MYCAN2, &frame );
		can_handler( MYCAN2, &frame );
	}

	check_softreset_sequence(delta_time_ms);

	led_seq_update(delta_time_ms);

	/* FallThrough test */
	/*twai_message_t msg = {0};

	if (simple_twai_recv(&stw0, &msg) == ESP_OK)
	{
		simple_twai_send(&stw1, &msg);
	}

	if (simple_twai_recv(&stw1, &msg) == ESP_OK)
	{
		simple_twai_send(&stw0, &msg);
	}*/
}

/* Port from arduino */
void yieldIfNecessary(void)
{
	static uint64_t lastYield = 0;
	uint64_t now = esp_timer_get_time() / 1000u;

	if ((now - lastYield) > 2000) {
		lastYield = now;
		vTaskDelay(5);  //delay 1 RTOS tick
	}
}

void can_filter(void *pv_params)
{
	// 1. Subscribe this specific task to the TWDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

	while (1) {
		simple_twai_update(&stw0);
		simple_twai_update(&stw1);

		can_bridge_main_loop();

		/* Give control to other tasks immediately. Must be level 0
		 * task to prevent starvation of IDLE task */
		esp_task_wdt_reset();
		yieldIfNecessary();
	}
}

/******************************************************************************
 * MAIN
 *****************************************************************************/
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

struct simple_twai stw0;
struct simple_twai stw1;

void app_main(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	//rescue_start(); /* Start RESCUE SERVER */

	stw0.id = 0;
	stw0.tx = GPIO_NUM_14;
	stw0.rx = GPIO_NUM_15;
	simple_twai_init(&stw0);

	stw1.id = 1;
	stw1.tx = GPIO_NUM_18;
	stw1.rx = GPIO_NUM_19;
	simple_twai_init(&stw1);

	led_seq_init();

	/* Force 2011 leaf */
	//My_Leaf = 0;

	assert(xTaskCreate(can_filter, "can_filter", 4096, NULL, 1, NULL) ==
		pdTRUE);
}
