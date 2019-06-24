// Based on ESP_MCP3201_SPI


#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include "wifi_params.h"
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

WiFiUDP udp;
const int udp_recv_port = 45990; // for command&control
const int udp_target_port = 45990; // sound transfer
const IPAddress IP_target_device(192, 168, 0, 13);
const IPAddress IP_target_PC(192, 168, 0, 2);
IPAddress IP_target = IP_target_device;

// Pin definitions: 
const int scePin = D8; //15;   	// SCE - Chip select
/* HW definition of alternate function:
static const uint8_t MOSI  = 13; D7 on nodemcu
static const uint8_t MISO  = 12; D6 on nodemcu
static const uint8_t SCK   = 14; D5 on nodemcu
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

int enable_highpass_filter = 0;

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
	xxxBA987 65432101
	We want 
	76543210 0000BA98

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
}

void ota_onprogress(unsigned int sz, unsigned int total)
{
	Serial.print("OTA: "); Serial.print(sz); Serial.print("/"); Serial.print(total);
	Serial.print("="); Serial.print(100*sz/total); Serial.println("%%");
}

void ota_onerror(ota_error_t err)
{
	Serial.print("OTA ERROR:"); Serial.println((int)err);
}


void setup(void)
{ 
	Serial.begin(115200);
	Serial.println("I was built on " __DATE__ " at " __TIME__ "");

	WiFi.setOutputPower(10); // reduce power to 10dBm = 10mW
	WiFi.mode(WIFI_STA);

	WiFi.begin(ssid, password);

	Serial.print("Connecting to wifi SSID ");
	Serial.print(ssid);
	// Wait for connection
	int now = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - now) < 10000) {
		delay(500);
		Serial.print(".");
	}

	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("failed to connect to home wifi, trying softAP");
		// Could not connect to wifi network in 5 sec? We must not be at home. Connect to softAP
		WiFi.begin(softap_ssid, softap_password);
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
	ArduinoOTA.setHostname("bb-xmit");
	ArduinoOTA.begin();

	spiBegin(); 
	
	timer1_isr_init();
	timer1_attachInterrupt(sample_isr);
	timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
	timer1_write(clockCyclesPerMicrosecond() / 16 * 50); //50us = 20kHz sampling freq

	Serial.println("setup done");

	udp.begin(udp_recv_port);
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

uint8_t *delta7_sample(uint16_t last, uint16_t *readptr, uint8_t *writeptr)
{
	const uint8_t lowbyte1 = *((uint8_t *)readptr);
	const uint8_t highbyte1 = *((uint8_t *)readptr+1);
	const uint16_t val = *readptr;

	const int32_t diff = val - last;
	if (diff > -64 && diff < 64) {
		// 7bit delta possible
		// Encode the delta as "sign and magnitude" format. 
		// CSMMMMMM (compressed signed magnitude^6)
		int8_t out = 0x80 | ((diff < 0) ? 0x40 : 0x0) | abs(diff);
		*writeptr++ = out;
	} else {
		// 7bit delta impossible, output as-is
		*writeptr++ = highbyte1;
		*writeptr++ = lowbyte1;
	}

	return writeptr;
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
		uint8_t *writeptr = (uint8_t *)(&adc_buf[!current_adc_buf][0]);
		uint16_t *readptr;
		uint16_t last = 0;
		for (unsigned int i = 0; i < number_of_samples; i++) {
			readptr = &adc_buf[!current_adc_buf][i];
			int32_t val = *readptr;
			int32_t rectified;

			if (enable_highpass_filter) {
				*readptr = filterloop(val) + 2048;
				val = *readptr;
			}

			rectified = abs(val - silence_value);

			accum_silence += val;
			envelope_value += rectified;

			// delta7-compress the data
			writeptr = delta7_sample(last, readptr, writeptr);
			last = val;
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
			udp.write((const uint8_t *)(&adc_buf[!current_adc_buf][0]), writeptr - (uint8_t *)&adc_buf[!current_adc_buf][0]);
			udp.endPacket();
		}
		send_samples_now = 0;
		Serial.print("Silence val "); Serial.print(silence_value); Serial.print(" envelope val "); Serial.print(envelope_value);	
		Serial.print("delay "); Serial.print(millis() - now);
		Serial.println("");
	}	

	if (udp.parsePacket()) {
		// Command and control packets
		char buf[32];
		char *ptr = &buf[0];
		udp.read(&buf[0], 31);
		buf[31] = 0;
#define MATCHSTR(X,Y) !strncmp(X, Y, strlen(Y))
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
		if (MATCHSTR(buf, "target PC")) {
			// Direct sound to PC
			IP_target = IP_target_PC;
			udp.print("target PC");
		} else if (MATCHSTR(buf, "target dev")) {
			// Direct sound to device
			IP_target = IP_target_device;
			udp.print("target dev");
		} else if (MATCHSTR(buf, "threshold ")) {
			// Modify envelope threshold
			ptr += strlen("threshold ");
			envelope_threshold = atoi(ptr);
			udp.print("threshold "); udp.println(envelope_threshold);
			
		} else if (MATCHSTR(buf, "sendnow")) {
			send_sound_util = millis() + 15000;
			udp.print("sending for 15 sec");
		} else if (MATCHSTR(buf, "filter")) {
			enable_highpass_filter = !enable_highpass_filter;
			if (enable_highpass_filter) {
				udp.print("enabled");
			} else {
				udp.print("disabled");
			}
			udp.println(" highpass filter");
		} else {
			udp.print("unknown command "); udp.println(buf);
		}
		udp.endPacket();
	}

	// If not sending anything, add a delay to enable modem sleep
	if (millis() > send_sound_util) {
		delay(10);
	}
}

