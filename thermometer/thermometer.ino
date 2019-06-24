#include <SPI.h>
#include <JeeLib.h>
#include <OneWire.h>
#include <avr/sleep.h>
#include <util/atomic.h>
#include "printf.h" 
#include "nRF24L01.h"
#include "RF24.h"

#define HAS_SOLAR_PANEL 0
#define HAS_RF24 1
#define HAS_LCD 0

#if HAS_LCD
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C	lcd(0x27,2,1,0,4,5,6,7); // 0x27 is the I2C bus address for an unmodified backpack
#endif

const int SCL_PIN = A5;
const int SDA_PIN = A4;
const int CE_PIN = 9;
const int CSN_PIN = 8;
const int BATTERY_PIN = A3;
const int SOLAR_PIN = A0;
const int SOLAR_RESISTOR_PIN = A1;
const int LED_YELLOW = 5;
const int LED_RED = 4;
const int DS18B20_PIN = 7;

OneWire ds(DS18B20_PIN);

#if HAS_RF24
RF24 radio(CE_PIN, CSN_PIN);
const uint64_t pipe_address = 0xF0F0F0F0F4LL;
#endif

static unsigned long last_ping_at = 0;

int init_failed = 0;

//const uint8_t thermometer_identification_letter = 'L'; // "living room"
//const uint8_t thermometer_identification_letter = 'E'; // "exterior"
//const uint8_t thermometer_identification_letter = 'B'; // "bedroom"
const uint8_t thermometer_identification_letter = 'K'; // "kid's bedroom"

ISR(WDT_vect)
{
	Sleepy::watchdogEvent();
}

int radio_send(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
#if HAS_RF24
	uint8_t payload[4] = { p0, p1, p2, p3 };
	digitalWrite(LED_YELLOW, 1);
	last_ping_at = millis();
	delayMicroseconds(5000);
	bool ok = radio.write(payload, 4);
	if (ok) 
		Serial.println("send ok");
	else
		Serial.println("send KO");
	
	digitalWrite(LED_YELLOW, 0);

	if (!ok) {
		digitalWrite(LED_RED, 1);
		return -1;
	}
	digitalWrite(LED_RED, 0);
#endif
	return 0;
}


int radio_send_bat(uint16_t bat)
{
	return radio_send('B', thermometer_identification_letter, (bat >> 8) & 0xFF, bat & 0xFF);
}

int radio_send_panel(uint16_t panel_voltage)
{
	return radio_send('S', thermometer_identification_letter, (panel_voltage >> 8) & 0xFF, panel_voltage & 0xFF);
}

int radio_send_current(uint16_t panel_voltage, uint16_t panel_resistor_voltage)
{
	// Panel -(-> SOLAR_PIN) - resistor (-> SOLAR_RESISTOR_PIN) - circuit
	// So the current is the voltage across the resistor, which is panel_voltage - panel_resistor_voltage
	// It's not supposed to be negative but I've observed -1 in some cases, so make the difference signed.
	int16_t value = panel_voltage - panel_resistor_voltage;

	// Resistor is set to 22 ohm
	// U = R.I so I = U / 22, send it *1000 to see something
	value = 1000 * (value  * 3.3f / 1024) / 22;
	return radio_send('C', thermometer_identification_letter, (value >> 8) & 0xFF, value & 0xFF);
}

void setup(){
	printf_begin();
	Serial.begin(57600);

#if HAS_RF24
	// Radio init
	radio.begin();
	radio.powerDown();
	radio.setRetries(15, 15);
	radio.setChannel(95);
	radio.setCRCLength(RF24_CRC_16);
	radio.setPayloadSize(sizeof(unsigned long));
	radio.setPALevel(RF24_PA_MAX);
	radio.setDataRate(RF24_250KBPS);
 	radio.setAutoAck(true);
	radio.openWritingPipe(pipe_address);
	radio.openReadingPipe(1, pipe_address);

	radio.printDetails();

	if ((radio.getDataRate() != RF24_250KBPS) ||
		(radio.getCRCLength() != RF24_CRC_16) /*|| 
		(radio.getChannel() != 95)*/) {
		// failed to initialize radio
		init_failed = 1;
	}
#endif

#if HAS_LCD
	lcd.begin (16,2); // for 16 x 2 LCD module
	lcd.setBacklightPin(3,POSITIVE);
	lcd.setBacklight(HIGH);
	lcd.home();
	lcd.print("Hello");
#endif

	pinMode(LED_YELLOW, OUTPUT);
	pinMode(LED_RED, OUTPUT);
	digitalWrite(LED_YELLOW, 1);
	digitalWrite(LED_RED, 1);
	delay(100);
	digitalWrite(LED_RED, 0);
	delay(100);
	digitalWrite(LED_RED, 1);
	delay(100);
	digitalWrite(LED_RED, 0);
	delay(100);
	digitalWrite(LED_RED, 1);
	delay(100);
	digitalWrite(LED_RED, 0);
	digitalWrite(LED_YELLOW, 0);

#if HAS_RF24
	radio.powerDown();
#endif
}

float battery_voltage(uint16_t battery_level)
{
	float lvl = (float)battery_level * 3.3 / 65536.0;
	return lvl;
}

void loop() 
{
	int red = 0;
	while (init_failed) {
			digitalWrite(LED_RED, red);
			red = !red;
			// Christmas tree if init failed
			digitalWrite(LED_YELLOW, 1);
			delay(50);
			digitalWrite(LED_YELLOW, 0);
			delay(50);
			digitalWrite(LED_YELLOW, 1);
			delay(50);
			digitalWrite(LED_YELLOW, 0);
			delay(50);
	}

	uint16_t battery_level = analogRead(BATTERY_PIN);
#if HAS_SOLAR_PANEL
	uint16_t panel_voltage = analogRead(SOLAR_PIN);
	uint16_t resistor_voltage = analogRead(SOLAR_RESISTOR_PIN);
#endif
#if HAS_RF24
	radio.powerUp();
#endif

	bool fail = radio_send_bat(battery_level);
#if HAS_SOLAR_PANEL
	fail &= radio_send_panel(panel_voltage);
	fail &= radio_send_current(panel_voltage, resistor_voltage);
#endif

	uint8_t addr[8];
	uint8_t data[12];
	uint8_t present;
	int16_t raw;
	int i = 5;
	while(!ds.search(addr) && i--) {
		Serial.println("No more addresses.");
		ds.reset_search();
		delay(250);
		if (!i) {
			// Indicate that we couldn't find the thermometer
			radio_send('T', thermometer_identification_letter, 0xFF, 0xFF);
			goto sleep;
		}
	}

	ds.reset();
	ds.select(addr);
	ds.write(0x44, 1);
	delay(1000);
	present = ds.reset();
	ds.select(addr);    
	ds.write(0xBE);

	for (int i = 0; i < 9; i++) {
		data[i] = ds.read();
	}

	
	if (data[8] != OneWire::crc8(data,8)) {
		Serial.println("ERROR: CRC didn't match");
		// Indicate that we read garbage
		radio_send('T', thermometer_identification_letter, 0xFF, 0xFF);
		goto sleep;
	} else {
		raw = (data[1] << 8) | data[0];
		byte cfg = (data[4] & 0x60);
		// at lower res, the low bits are undefined, so let's zero them
		if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
		else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
		else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
		//// default is 12 bit resolution, 750 ms conversion time
		printf("Temperature is %d\n", (int)(100.0*(float)raw/16.0));
	}

#if HAS_LCD
	lcd.home();
	lcd.print("Temperature");
	lcd.setCursor(0,1);
	lcd.print((float)raw/16.0);
#endif
	i = 100;

	while (i--) {
	    fail = radio_send('T', thermometer_identification_letter, (raw >> 8) & 0xFF, raw & 0xFF);
		if (!fail) {
			break;
		}

		Serial.println("radio message not sent, not sleeping");
		delay(2000);
	}
sleep:	
#if HAS_RF24
	radio.powerDown();
#endif
	Serial.println("going to sleep now");
	Serial.flush();
	Sleepy::loseSomeTime(32768L);
	Serial.println("waking up");

	for (int i = 1; i < 15L*60L*1000L / 32768L; i++) {
#if HAS_SOLAR_PANEL
		if (battery_voltage(battery_level) >= 1.4f) {
			// Battery is overcharged, try to waste energy!
			delay(10000);
		}
#endif
		if (!Sleepy::loseSomeTime(32768L)) {
			Serial.println("woken up by intr");
		}
	}
}



