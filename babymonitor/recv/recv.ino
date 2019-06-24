#include <Wire.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include "ESP8266mDNS.h"
#include <ArduinoOTA.h>

#include "wifi_params.h"

const IPAddress softAP_myaddress(192, 168, 0, 13);

const int mySDA = D7;
const int mySCL = D6;

const int AMPLI_MUTE_PIN = D2;
const int AMPLI_SHUTDOWN_PIN = D1;

const int RIGHT_BTN = D3;
const int LEFT_BTN = D4;
const int LED1 = D8;

const int udp_recv_port = 45990;
	
WiFiUDP udp;
TwoWire i2c;

#define NB_DATA_BUFS 5
uint16_t data_buf[NB_DATA_BUFS][700]; // data buffer, N buffered

unsigned int current_play_data_buf; // current data buf being played
unsigned int play_data_buf_pos; // position in the ADC data buffer
unsigned int current_recv_data_buf; // current data buf being received

bool play_waiting = true;
bool amplifier_stopped = false;
long play_waiting_at;

volatile bool left_btn_pressed;
volatile bool right_btn_pressed;

#define ICACHE_RAM_ATTR     __attribute__((section(".iram.text")))
#define twi_sda mySDA
#define twi_scl mySCL
#define twi_dcount 0
#define twi_clockStretchLimit 10
#define SDA_LOW()   (GPES = (1 << twi_sda)) //Enable SDA (becomes output and since GPO is 0 for the pin, it will pull the line low)
#define SDA_HIGH()  (GPEC = (1 << twi_sda)) //Disable SDA (becomes input and since it has pullup it will go high)
#define SDA_READ()  ((GPI & (1 << twi_sda)) != 0)
#define SCL_LOW()   (GPES = (1 << twi_scl))
#define SCL_HIGH()  (GPEC = (1 << twi_scl))
#define SCL_READ()  ((GPI & (1 << twi_scl)) != 0)

static void twi_delay(unsigned char v){
  unsigned int i;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
  unsigned int reg;
  for(i=0;i<v;i++) reg = GPI;
#pragma GCC diagnostic pop
} 

static inline ICACHE_RAM_ATTR bool twi_write_start(void) {
  SCL_HIGH();
  SDA_HIGH();
  if (SDA_READ() == 0) return false;
  SDA_LOW();
  return true;
}

static inline ICACHE_RAM_ATTR bool twi_write_stop(void){
  uint32_t i = 0;
  SCL_LOW();
  SDA_LOW();
  SCL_HIGH();
  while (SCL_READ() == 0 && (i++) < twi_clockStretchLimit); // Clock stretching
  SDA_HIGH();

  return true;
}

static inline ICACHE_RAM_ATTR bool twi_write_bit(bool bit) {
  uint32_t i = 0;
  SCL_LOW();
  if (bit) SDA_HIGH();
  else SDA_LOW();
  twi_delay(twi_dcount+1);
  SCL_HIGH();
  while (SCL_READ() == 0 && (i++) < twi_clockStretchLimit);// Clock stretching
  return true;
}

static inline ICACHE_RAM_ATTR bool twi_read_bit(void) {
  uint32_t i = 0;
  SCL_LOW();
  SDA_HIGH();
  twi_delay(twi_dcount+2);
  SCL_HIGH();
  while (SCL_READ() == 0 && (i++) < twi_clockStretchLimit);// Clock stretching
  bool bit = SDA_READ();
  return bit;
}

static inline ICACHE_RAM_ATTR bool twi_write_byte(unsigned char byte) {
  unsigned char bit;
  for (bit = 0; bit < 8; bit++) {
    twi_write_bit(byte & 0x80);
    byte <<= 1;
  }
  return !twi_read_bit();//NACK/ACK
}

static inline ICACHE_RAM_ATTR unsigned char twi_read_byte(bool nack) {
  unsigned char byte = 0;
  unsigned char bit;
  for (bit = 0; bit < 8; bit++) byte = (byte << 1) | twi_read_bit();
  twi_write_bit(nack);
  return byte;
}


unsigned char inline ICACHE_RAM_ATTR mytwi_writeTo(unsigned char address, unsigned char * buf, unsigned int len, unsigned char sendStop){
  unsigned int i;
  if(!twi_write_start()) return 4;//line busy
  if(!twi_write_byte(((address << 1) | 0) & 0xFF)) {
    if (sendStop) twi_write_stop();
    return 2; //received NACK on transmit of address
  }
  for(i=0; i<len; i++) {
    if(!twi_write_byte(buf[i])) {
      if (sendStop) twi_write_stop();
      return 3;//received NACK on transmit of data
    }
  }
  if(sendStop) twi_write_stop();
  i = 0;
  while(SDA_READ() == 0 && (i++) < 10){
    SCL_LOW();
    SCL_HIGH();
  }
  return 0;
}



static inline ICACHE_RAM_ATTR uint8_t DAC(uint16_t value) 
{
	/* value is 76543210 XXXXBA98
	per the datasheet for fast write:
	1 1 0 0 A2 A1 A0 0 <ACK> 0 0 PD1 PD0 D11 D10 D9 D8 <ACK> D7 D6 D5 D4 D3 D2 D1 D0 <ACK> 
	*/

	uint8_t buf[2] = { (value >> 8) & 0x0F, (value & 0xFF) };
	int ret = mytwi_writeTo(0x60, buf, 2, true);
	return ret;
}

void ICACHE_RAM_ATTR playsample_isr(void)
{
	if (play_waiting) {
		return;
	}

	DAC(data_buf[current_play_data_buf][play_data_buf_pos]);
	play_data_buf_pos++;
	if (play_data_buf_pos >= sizeof(data_buf[0])/sizeof(data_buf[0][0])) {
		play_data_buf_pos = 0;
		current_play_data_buf++;
		if (current_play_data_buf == NB_DATA_BUFS) {
			current_play_data_buf = 0;
		}

		if (current_play_data_buf == current_recv_data_buf) {
			play_waiting = true;
			play_waiting_at = micros();
		}
	}
}

void ota_onstart(void)
{
	// Disable timer when an OTA happens
	timer1_detachInterrupt();
	timer1_disable();
}

void ota_onprogress(unsigned int sz, unsigned int total)
{
	Serial.print("OTA: "); Serial.print(sz); Serial.print("/"); Serial.print(total);
	Serial.print("="); Serial.print(100*sz/total); Serial.println("%");
}

void ota_onerror(ota_error_t err)
{
	Serial.print("OTA ERROR:"); Serial.println((int)err);
}

void left_btn_intr()
{
	left_btn_pressed = 1;
}

void right_btn_intr()
{
	right_btn_pressed = 1;
}

void setup ( void ) 
{
	Serial.begin ( 115200 );
	Serial.println("I was built on " __DATE__ " at " __TIME__ "");
	
	i2c.begin(mySDA, mySCL);
	i2c.setClock(400000);

	WiFi.mode(WIFI_STA);
	bool found = false;
	int n = WiFi.scanNetworks();
	for (int i = 0; i < n; i++) {
		if (WiFi.SSID(i).equals(ssid)) {
			found = true;
			break;
		}
	}
	if (!found) {
		Serial.println("No wifi network found, starting softAP\n");
		WiFi.disconnect();
		WiFi.mode(WIFI_AP);
		WiFi.softAPConfig(softAP_myaddress, softAP_myaddress, IPAddress(255,255,255,0));
		WiFi.softAP(softap_ssid, softap_password);
	} else {
		WiFi.begin(ssid, password);
		WiFi.setSleepMode(WIFI_MODEM_SLEEP);
		Serial.print("Connecting to wifi");
		int now = millis();
		while (WiFi.status() != WL_CONNECTED) {
			delay(500);
			Serial.print(".");
		}
	}

	Serial.println ( "" );
	Serial.print ( "Cnnectd to " );
	Serial.println ( ssid );
	Serial.print ( "IP " );
	Serial.println ( WiFi.localIP() );

	ArduinoOTA.onStart(ota_onstart);
	ArduinoOTA.onError(ota_onerror);
	ArduinoOTA.onProgress(ota_onprogress);
	ArduinoOTA.setHostname("bb-recv");
	ArduinoOTA.begin();

	timer1_isr_init();
	timer1_attachInterrupt(playsample_isr);
	timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
	timer1_write(clockCyclesPerMicrosecond() / 16 * 50); //50us = 20 kHz sampling freq

	udp.begin(udp_recv_port);

	pinMode(AMPLI_MUTE_PIN, OUTPUT);
	pinMode(AMPLI_SHUTDOWN_PIN, OUTPUT);
	digitalWrite(AMPLI_SHUTDOWN_PIN, 0);
	digitalWrite(AMPLI_MUTE_PIN, 0);

	pinMode(LEFT_BTN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(LEFT_BTN), left_btn_intr, FALLING);
	pinMode(RIGHT_BTN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(RIGHT_BTN), right_btn_intr, FALLING);

	pinMode(LED1, OUTPUT);
	digitalWrite(LED1, 0);

}

int do_undelta7(const uint8_t *val, int sz, uint16_t *out)
{
    // Implement delta 7 decompression.
    // First bit = 0 <=> uncompressed 15 bits following 
    // First bit = 1 <=> 7 bits follow representing delta
    // must switch to big endian...
    uint16_t last = 0;
    uint8_t *ptr = (uint8_t *)&out[0];
    const uint8_t *start = ptr;
    for (int i = 0; i < sz; i++) {
        uint16_t *ptr16 = (uint16_t *)ptr;
        const int8_t firstbyte = val[i];
        if (firstbyte & 0x80) {
            // Delta7 compressed
            // byte is CSMMMMMM
            int8_t delta = firstbyte & 0x3F;
            if (firstbyte & 0x40) {
                delta = -delta;
            }
            const uint16_t value = last + delta;
            *ptr16 = value;
            ptr += 2;

            last = value;
        } else {
            // uncompressed -- switch bytes back to LE
            *ptr++ = val[i+1];
            *ptr++ = val[i];
            last = val[i+1] | val[i] << 8;
            i++;
        }
    }

    return ptr - start;

}

void loop ( void ) 
{
	ArduinoOTA.handle();
	int sz = udp.parsePacket();
	if (sz) {
		uint8_t buf[sz];
		udp.read(&buf[0], sz);
		current_recv_data_buf++;
		if (current_recv_data_buf == NB_DATA_BUFS) {
			current_recv_data_buf = 0;
			if (current_recv_data_buf == current_play_data_buf && !play_waiting) {
				Serial.println("buffer overflow when receiving");
			}
		}
		do_undelta7(buf, sz, &data_buf[current_recv_data_buf][0]);
		if (play_waiting) {
			Serial.print("Restarting play, was waiting (us)"); Serial.println(micros() - play_waiting_at);
			// Re-enable *then* unmute in that order to avoid pops
			digitalWrite(AMPLI_SHUTDOWN_PIN, 1);
			digitalWrite(AMPLI_MUTE_PIN, 1);
			play_waiting = false;
			amplifier_stopped = false;
			digitalWrite(LED1, 1);
		}

		Serial.println("");
	}

	// If not playing anything, but amplifier is still up
	if (!amplifier_stopped && play_waiting) {
		if ((micros() - play_waiting_at) > 2000 * 1000) {
			// If nothing has been played for two seconds, shut down the amplifier 
			Serial.println("Shutting down amplifier!");
			digitalWrite(AMPLI_SHUTDOWN_PIN, 0);
			digitalWrite(AMPLI_MUTE_PIN, 0);
			amplifier_stopped = true;
			digitalWrite(LED1, 0);
		}
	}

	if (left_btn_pressed) {
			left_btn_pressed = 0;
			digitalWrite(AMPLI_MUTE_PIN, 0);
			digitalWrite(AMPLI_SHUTDOWN_PIN, 0);
	}

	if (right_btn_pressed) {
			digitalWrite(AMPLI_SHUTDOWN_PIN, 1);
			digitalWrite(AMPLI_MUTE_PIN, 1);
			const IPAddress bcast(255, 255, 255, 255);
			udp.beginPacket(bcast, 45990);
			udp.write("sendnow");
			udp.endPacket();
			right_btn_pressed = 0;
	} 

	// If the amplifier is stopped, add a delay for power saving
	if (amplifier_stopped) {
		delay(10);
	}

}
