// Based on ESP_MCP3201_SPI


#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include "wifi_params.h"
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

WiFiUDP udp;
const int udp_target_port = 45990;
const IPAddress IP_target(192,168,0,2);

// Pin definitions: 
const int scePin = 15;   	// SCE - Chip select
/* HW definition of alternate function:
static const uint8_t MOSI  = 13;
static const uint8_t MISO  = 12;
static const uint8_t SCK   = 14;
*/
/*  Hardware: 
      MCP3201 Pin   ---------------- ESP8266 Pin
-       1-VREF      ----------------  3,3V
-       2-IN+       ----------------  ANALOG SIGNAL +
-       3-IN-       ----------------  ANALOG SIGNAL -
-       4-GND       ----------------  GND
-       5-CS        ----CS----------  GPIO15/CS (PIN 19)
-       6-Dout(MISO)----MISO--------  GPIO12/MISO (PIN 16)
-       7-CLK       ----SCLK--------  GPIO14 (PIN 17)
-       8-VDD       ----------------  3.3V  
*/

uint16_t adc_buf[2][700]; // ADC data buffer, double buffered
int current_adc_buf; // which data buffer is being used for the ADC (the other is being sent)
unsigned int adc_buf_pos; // position in the ADC data buffer
int send_samples_now; // flag to signal that a buffer is ready to be sent

#define SILENCE_EMA_WEIGHT 1024
#define ENVELOPE_EMA_WEIGHT 2
int32_t silence_value = 2048; // computed as an exponential moving average of the signal
uint16_t envelope_threshold = 150; // envelope threshold to trigger data sending

uint32_t send_sound_util = 0; // date until sound transmission ends after an envelope threshold has triggered sound transmission

int enable_highpass_filter = 1;

static inline void setDataBits(uint16_t bits) {
    const uint32_t mask = ~((SPIMMOSI << SPILMOSI) | (SPIMMISO << SPILMISO));
    bits--;
    SPI1U1 = ((SPI1U1 & mask) | ((bits << SPILMOSI) | (bits << SPILMISO)));
}

void spiBegin(void) 
{
  SPI.begin();
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV8); 
  SPI.setHwCs(1);
  setDataBits(16);
}

#define ICACHE_RAM_ATTR     __attribute__((section(".iram.text")))
/* SPI code based on the SPI library */
static inline ICACHE_RAM_ATTR uint16_t transfer16(void) {
	union {
		uint16_t val;
		struct {
			uint8_t lsb;
			uint8_t msb;
		};
	} out;


	// Transfer 16 bits at once, leaving HW CS low for the whole 16 bits 
	while(SPI1CMD & SPIBUSY) {}
	SPI1W0 = 0;
	SPI1CMD |= SPIBUSY;
	while(SPI1CMD & SPIBUSY) {}

	/* Follow MCP3201's datasheet: return value looks like this:
	xxCBA987 65432101
	We want 
	76543210 000CBA98

	So swap the bytes, select 12 bits starting at bit 1, and shift right by one.
	*/

	out.val = SPI1W0 & 0xFFFF;
	uint8_t tmp = out.msb;
	out.msb = out.lsb;
	out.lsb = tmp;

	out.val &= (0x0FFF << 1);
	out.val >>= 1;
	return out.val;
}

void ICACHE_RAM_ATTR sample_isr(void)
{
	uint16_t val;

	// Read a sample from ADC
	val = transfer16();
	adc_buf[current_adc_buf][adc_buf_pos] = val & 0xFFF;
	adc_buf_pos++;

	// If the buffer is full, signal it's ready to be sent and switch to the other one
	if (adc_buf_pos > sizeof(adc_buf[0])/sizeof(adc_buf[0][0])) {
		adc_buf_pos = 0;
		current_adc_buf = !current_adc_buf;
		send_samples_now = 1;
	}
}

void ota_onstart(void)
{
	// Disable timer when an OTA happens
	timer1_detachInterrupt();
	timer1_disable();

	// Disable SPI XXX explain why -- no effect on issue
	SPI.end();
}

void ota_onprogress(unsigned int sz, unsigned int total)
{
	Serial.print("OTA: "); Serial.print(sz); Serial.print("/"); Serial.print(total);
	Serial.print("="); Serial.print(100*sz/total); Serial.println("");
}

void ota_onerror(ota_error_t err)
{
	Serial.print("OTA ERROR:"); Serial.println((int)err);
}


void setup(void)
{ 
	Serial.begin(115200);
	Serial.println("I was built on " __DATE__ " at " __TIME__ "");

	WiFi.begin ( ssid, password );
	IPAddress myip(192, 168, 0, 32);
	IPAddress gw(192, 168, 0, 1);
	IPAddress subnet(255, 255, 255, 0);
	WiFi.config(myip, gw, subnet);
	Serial.print("Connecting to wifi");
	// Wait for connection
	while ( WiFi.status() != WL_CONNECTED ) {
		delay ( 500 );
		Serial.print ( "." );
	}

	Serial.println ( "" );
	Serial.print ( "Cnnectd to " );
	Serial.println ( ssid );
	Serial.print ( "IP " );
	Serial.println ( WiFi.localIP() );

	ArduinoOTA.onStart(ota_onstart);
	ArduinoOTA.onError(ota_onerror);
	ArduinoOTA.onProgress(ota_onprogress);
	ArduinoOTA.begin();

	spiBegin(); 
	
	timer1_isr_init();
	timer1_attachInterrupt(sample_isr);
	timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
	timer1_write(clockCyclesPerMicrosecond() / 16 * 80); //80us = 12.5kHz sampling freq

	Serial.println("setup done");
}

#pragma GCC push_options
#pragma GCC optimize("O3")

/* Digital filter designed by mkfilter/mkshape/gencode   A.J. Fisher
   Command line: ./mkfilter -Bu -Hp -o 5 -a 0.012 -l */
// Highpass, Fc=150Hz, 5th order butterworth filter

#define NZEROS 5
#define NPOLES 5
#define GAIN   1.129790960e+00f

static float xv[NZEROS+1], yv[NPOLES+1];

static float filterloop(float input)
  { for (;;)
      { xv[0] = xv[1]; xv[1] = xv[2]; xv[2] = xv[3]; xv[3] = xv[4]; xv[4] = xv[5]; 
        xv[5] = input / GAIN;
        yv[0] = yv[1]; yv[1] = yv[2]; yv[2] = yv[3]; yv[3] = yv[4]; yv[4] = yv[5]; 
        yv[5] =   (xv[5] - xv[0]) + 5 * (xv[1] - xv[4]) + 10 * (xv[3] - xv[2])
                     + (  0.7834365141f * yv[0]) + ( -4.1083230157f * yv[1])
                     + (  8.6224512099f * yv[2]) + ( -9.0535899276f * yv[3])
                     + (  4.7560230574f * yv[4]);
        return yv[5];
      }
  }

void loop() 
{
	ArduinoOTA.handle();
	if (send_samples_now) {
		/* We're ready to send a buffer of samples over wifi. Decide if it has to happen or not,
		   that is, if the sound level is above a certain threshold. */

		// Update silence and envelope computations
		uint16_t number_of_samples = sizeof(adc_buf[0])/sizeof(adc_buf[0][0]);
		int32_t accum_silence = 0;
		int32_t envelope_value = 0;

		int32_t now = millis();
		for (unsigned int i = 0; i < number_of_samples; i++) {
			int32_t val = adc_buf[!current_adc_buf][i];
			int32_t rectified;

			if (enable_highpass_filter) {
				adc_buf[!current_adc_buf][i] = filterloop(val) + 2048;
			}
			val = adc_buf[!current_adc_buf][i];

			rectified = abs(val - silence_value);

			accum_silence += val;
			envelope_value += rectified;
		}
		accum_silence /= number_of_samples;
		envelope_value /= number_of_samples;
		silence_value = (SILENCE_EMA_WEIGHT * silence_value + accum_silence) / (SILENCE_EMA_WEIGHT + 1);
		envelope_value = envelope_value;

		if (envelope_value > envelope_threshold) {
			send_sound_util = millis() + 15000; 
		} 

		if (millis() < send_sound_util) {
			udp.beginPacket(IP_target, udp_target_port);
			udp.write((const uint8_t *)(&adc_buf[!current_adc_buf][0]), sizeof(adc_buf[0]));
			udp.endPacket();
		}
		send_samples_now = 0;
		Serial.print("Silence val "); Serial.print(silence_value); Serial.print(" envelope val "); Serial.print(envelope_value);	
		Serial.print("delay "); Serial.print(millis() - now);
		Serial.println("");
	}	 
}
#pragma GCC pop_options
