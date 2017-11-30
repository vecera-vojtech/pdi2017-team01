#include <WiFi.h>
#include <PubSubClient.h>
#include <dht.h>
#include "main.h"
// Wemos D1 mini pins compared to ESP GPIO
// source: https://www.wemos.cc/product/d1-mini.html
// Wemos|ESP
// TX   TXD
// RX   RXD
// A0   A0
// D0   GPIO16, doesn't have interrupt/pwm
// D1   GPIO5/SCL
// D2   GPIO4/SDA
// D3   GPIO0, 10k PU, ESP special (set to 0 during reset to enter bootloader)
// D4   GPIO2, 10k PU, LED, ESP special (should be high during reset)
// D5   GPIO14/SCK
// D6   GPIO12/MISO
// D7   GPIO13/MOSI
// D8   GPIO15/SS, 10k PD, ESP special (should be low during reset)
// G    GND
// 5V   -
// 3V3  3.3V
// RST  RST

// note: using ESP GPIO numbers rather than Wemos for easier portability to other ESP versions

// setup instances for Wifi and 1Wire
WiFiClient espClient;
PubSubClient client(espClient);
dht DHT;

// variables
char mqtt_msg[50];
uint16_t connect_cnt = 0;
uint16_t sync_cnt = 0;

unsigned char current_pc_status = 255;
unsigned char last_read_status = 255;
unsigned char last_published_status = 255;
long last_status_unstable_ms = 0;

float current_temp = -127.0;
float last_published_temp = -127.0;
long last_temp_request_ms = 0;

long last_published_ms = 0;
PubData_e data_to_publish = PUB_DATA_PC_STATUS;
unsigned char is_syncing = 0;

///////////////////////////////////////////////////////////////////////////////
/// Init functions
///////////////////////////////////////////////////////////////////////////////
void Wifi_Connect(void) {
	Serial.println(); // before this we have crap from ESP bootloader
	Serial.print("Connecting to ");
	Serial.println(WIFI_SSID);
	// connect to Wifi network
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	while (WiFi.status() != WL_CONNECTED)	{
		delay(500);
		Serial.print(".");
	}
	Serial.println();
	Serial.print("WiFi connected! IP: ");
	Serial.println(WiFi.localIP());
}

///////////////////////////////////////////////////////////////////////////////
/// Setup and main loop
///////////////////////////////////////////////////////////////////////////////
void setup() {
	Serial.begin(115200, SERIAL_8N1);
	pinMode(GPIO_OUT_PWSW, OUTPUT);
	digitalWrite(GPIO_OUT_PWSW, OUT_STATE_INACTIVE);
	pinMode(GPIO_OUT_RTSW, OUTPUT);
	digitalWrite(GPIO_OUT_RTSW, OUT_STATE_INACTIVE);
	pinMode(GPIO_IN_STATUS, INPUT);
	delay(500);
	// connect to Wifi
	Wifi_Connect();
	// setup MQTT
	client.setServer(CLOUDMQTT_SERVER, CLOUDMQTT_PORT);
	client.setCallback(Subscription_Callback);
}

void loop() {
	// handle MQTT connection
	if (!client.connected()) {
		connect_cnt++;
		Mqtt_Reconnect();
		last_published_ms = last_status_unstable_ms = millis(); // init time here, it takes a while to connect
	}
	client.loop();

	// handle temperature reading
	if ((millis() - last_temp_request_ms) > TEMP_REFRESH_MS) {
		// get temperature from first index (since we have only one sensor)
	 	int chk = DHT.read11(GPIO_TEMP);
		Serial.print("Temperature read: ");
		Serial.println(DHT.temperature);
		current_temp = DHT.temperature;

		// request new temperature conversion, which can take up to 750 ms (DS18B20)
		// we don't wait here for that since we setup async conversion
		last_temp_request_ms = millis();
	}

	long now_ms = millis();

	// read current input status
	unsigned char tmp_status = digitalRead(GPIO_IN_STATUS);
	// invert if needed
	if (IN_STATUS_INVERTED)
		tmp_status = !tmp_status;

	// check if unstable
	if (tmp_status != last_read_status) {
		last_status_unstable_ms = now_ms;
		last_read_status = tmp_status;
	}

	// check if debounce period elapsed, save new status
	if ((now_ms - last_status_unstable_ms) > DEBOUNCE_STATUS_MS) {
		if (current_pc_status != tmp_status) {
			current_pc_status = tmp_status;
			Serial.print("PC status ");
			Serial.println(current_pc_status);
		}
	}

	// limit minimal MQTT sending interval, publish data
	if ((now_ms - last_published_ms) > PUB_MIN_MS) {
		// send on PC status change
		if (current_pc_status != last_published_status) {
			Publish_PcStatus(current_pc_status);
			last_published_ms = now_ms;
		}	else if (fabs(current_temp - last_published_temp) > PUB_TEMP_THRESHOLD) {
			// or on temperature change
			Publish_Temperature(current_temp);
			last_published_ms = now_ms;
		}	else if (is_syncing) {
		// check if we were syncing data
			char tmp_str[20];

			// increment counter and publish status
			sync_cnt++;
			snprintf(tmp_str, sizeof(tmp_str), "Synced(%d)", sync_cnt);
			Publish_Connection(tmp_str);
			is_syncing = 0; // sync complete at this point, everything that needed to be sent is above
		} else if ((now_ms - last_published_ms) > PUB_PERIODIC_MS) {
		// send periodically
			// cycle through data to publish periodically
			if (data_to_publish >= PUB_DATA_MAX) data_to_publish = PUB_DATA_PC_STATUS;

			if (data_to_publish == PUB_DATA_PC_STATUS) Publish_PcStatus(current_pc_status);
			else if (data_to_publish == PUB_DATA_TEMP) Publish_Temperature(current_temp);

			data_to_publish = (PubData_e)(data_to_publish + 1);
			last_published_ms = now_ms;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
/// MQTT functions
///////////////////////////////////////////////////////////////////////////////
void Mqtt_Reconnect() {
	char tmp_str[30];
	uint32_t millis_start = millis();

	// Loop until we're reconnected
	while (!client.connected()) {
		Serial.print("Attempting MQTT connection...");
		// Attempt to connect
		if (client.connect(MQTT_CLIENT_ID, CLOUDMQTT_USER, CLOUDMQTT_PASS)) {
			// Once connected, publish an announcement...
			snprintf(tmp_str, sizeof(tmp_str), "%s", MQTT_CLIENT_ID);
			Serial.println(tmp_str);
			Publish_Connection(tmp_str);
			// ... and resubscribe
			client.subscribe(TOPIC_IN_PC_STATE);
			client.subscribe(TOPIC_IN_PC_RESET);
			break;
		} else {
			Serial.print("failed, rc=");
			Serial.print(client.state());
			Serial.println(" try again in 5 seconds");
			// Wait 5 seconds before retrying
			delay(5000);
		}

		// restart ESP if we cannot connect for too long
		if ((millis() - millis_start) > 2 * 60000) {
			Serial.println("Cannot connect to MQTT, restarting...");
			ESP.restart();
		}
	}
}

void Publish_Connection(char * text) {
	// report to terminal for debug
	snprintf(mqtt_msg, sizeof(mqtt_msg), "Publishing %s %s", TOPIC_OUT_CONN, text);
	Serial.println(mqtt_msg);
	// publish to MQTT server
	client.publish(TOPIC_OUT_CONN, text, MQTT_USE_RETAIN);
}

void Publish_PcStatus(unsigned char pc_status) {
	// report to terminal for debug
	snprintf(mqtt_msg, sizeof(mqtt_msg), "%s", String(pc_status).c_str());
	Serial.print("Publishing /status ");
	Serial.println(mqtt_msg);
	// publish to MQTT server
	client.publish(TOPIC_OUT_PC_STATUS, mqtt_msg, MQTT_USE_RETAIN);
	// save last published status
	last_published_status = pc_status;
}

void Publish_Temperature(float temp) {
	int16_t temp_i16;
	char temp_string[10];
	// temperature value is in degrees C as float, we'll report with
	// only one decimal since accuracy of sensor is +-0.5 degC
	// convert to integer to avoid float sprintf issues
	if (temp >= 0.0)
		temp_i16 = (temp + 0.05f); // add 0.05f to round up
	else
		temp_i16 = (temp - 0.05f);
	// write integer temperature to string
	snprintf(temp_string, sizeof(temp_string), "%d", temp_i16);
	// report to terminal for debug
	snprintf(mqtt_msg, sizeof(mqtt_msg), "Publishing /temp %s", temp_string);
	Serial.println(mqtt_msg);
	// publish to MQTT server
	client.publish(TOPIC_OUT_TEMP, temp_string, MQTT_USE_RETAIN);
	// remember last sent
	last_published_temp = temp;
}

void Subscription_Callback(char* topic, unsigned char* payload, unsigned int length) {
	// report to terminal for debug
	Serial.print("MQTT msg arrived: ");
	Serial.print(topic);
	Serial.print(" ");
	for (int i = 0; i < length; i++)
		Serial.print((char)payload[i]);
	Serial.println();

	// check if we received our IN topic for state change
	if (strcmp(TOPIC_IN_PC_STATE, topic) == 0) {
		unsigned char target_state = current_pc_status;
		// now the switch in the app is sending 'true' or 'false'
		// check for valid values
		if ((char)payload[0] == 't') {
				Serial.println("/state true");
			target_state = 1;
		} else if ((char)payload[0] == 'f') {
				Serial.println("/state false");
			target_state = 0;
		}

		Serial.print("Checking state! target|current=");
		Serial.print(target_state);
		Serial.print("|");
		Serial.println(current_pc_status);
		// toggle output (PC power switch) if we need to change state
		if (target_state != current_pc_status) {
			if (target_state == 1)
				TogglePc(TOGGLE_ON);
			else
				TogglePc(TOGGLE_OFF);
		}


	} else if (strcmp(TOPIC_IN_PC_RESET, topic) == 0) {
		unsigned char reset = 0;
		if ((char)payload[0] == 't') {
				Serial.println("/reset");
				reset = 1;
		}

		Serial.print("Checking state! current=");
		Serial.println(current_pc_status);
		if (current_pc_status == 1) {
			ResetPc();
			reset = 0;
		}
	}
}

void TogglePc(int state) {
	digitalWrite(GPIO_OUT_PWSW, OUT_STATE_ACTIVE);
	delay(state);
	digitalWrite(GPIO_OUT_PWSW, OUT_STATE_INACTIVE);
	Serial.println("Power switched!");
}
void ResetPc(void) {
	/*
	 * TODO: Test reset functionality
	 */
	digitalWrite(GPIO_OUT_RTSW, OUT_STATE_ACTIVE);
	delay(TOGGLE_ON);
	digitalWrite(GPIO_OUT_RTSW, OUT_STATE_INACTIVE);
	Serial.println("Reset!");
}
