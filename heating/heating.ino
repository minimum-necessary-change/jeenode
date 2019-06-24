#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <user_interface.h>
#include "wifi_params.h"

// This device uses an ESP201, a device with 512kB flash. OTAs are impossible without hacking the Arduino framewark.

ESP8266WebServer websrv (80);
WiFiUDP udp;
const int udp_port = 2222;

const int chaudiere = 14;
const int pushbtn = 12;

int pwm;

unsigned long int forced_heating_until = 0;
unsigned long int send_next_ping_at = 15*60*1000;

bool pushbtn_pressed = false;

static void handleRoot() {
	char temp[1024];
	int sec = millis() / 1000;
	int min = sec / 60;
	int hr = min / 60;

	snprintf ( temp, 1024,

"<html><head><title>ESP8266</title></head><body>\
    <h1>Hello from ESP8266!</h1>\
    <p>Built on %s at %s</p><p>Uptime: %02d:%02d:%02d = %d ms</p>\
	<p>Current power is %d, %sforced until %d, %d minutes from now</p> \
  </body></html>",

		__DATE__, __TIME__, hr, min % 60, sec % 60, millis(),
		pwm, (forced_heating_until ? "" : "not "), forced_heating_until, (forced_heating_until - millis()) / 60 / 1000
	);
	websrv.send ( 200, "text/html", temp );
}

static void pushbtn_intr(void)
{
	if (!digitalRead(pushbtn)) {
		pushbtn_pressed = true;
	}
}

void setup ( void ) {
	pinMode ( chaudiere, OUTPUT );
	pinMode(pushbtn, INPUT_PULLUP);
	digitalWrite(chaudiere, 1);
	Serial.begin ( 57600 );
	
	WiFi.setOutputPower(10); // reduce power to 10dBm = 10mW
	WiFi.mode(WIFI_STA);
	WiFi.begin ( ssid, password );
	Serial.println ( "" );

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

	udp.begin(udp_port);
	websrv.on ( "/", handleRoot );

	websrv.begin();
		  
	attachInterrupt(digitalPinToInterrupt(pushbtn), pushbtn_intr, FALLING);
}

void udp_send(const char *str)
{
	udp.beginPacket(udp.remoteIP(), udp.remotePort());
	udp.write(str);
	udp.endPacket();
}

void change_pwm(const void *buf)
{
	char str[255];
	int duty_cycle = atoi((const char *)buf);
	Serial.print("Recvd UDP req to set duty cycle to ");
	Serial.println(duty_cycle);

	if (millis() < forced_heating_until) {
		return;
	}

	if (duty_cycle >= 0 && duty_cycle <= 100) {
		pwm = duty_cycle;
		sprintf(str, "Setting duty cycle to %d\n", duty_cycle);
		udp_send(str);
		if (duty_cycle == 100) {
			digitalWrite(chaudiere, 0);
		} else {
			digitalWrite(chaudiere, 1);
		}
	} else {
		sprintf(str, "Req.invalid duty cycle %d\n", duty_cycle);
		udp_send(str);
	}

}

static void udp_send_status_report(void)
{
	char str[50];
	sprintf(str, "PWM %d", pwm);
	udp_send(str);
}

static void parse_cmd(const char *buf)
{
	if (!strncmp(buf, "PWM ", 4)) {
		buf += 4;
		change_pwm(buf);
	} else if (!strncmp(buf, "STATUS", 6)) {
		udp_send_status_report();
	} else if (!strncmp(buf, "ON", 2)) {
		change_pwm("100");
	} else if (!strncmp(buf, "OFF", 3)) {
		change_pwm("0");
	} else if (!strncmp(buf, "FORCEOFF", 8)) {
		change_pwm("0");
		forced_heating_until = 0;
	} 
}

void loop ( void ) {
	websrv.handleClient();

	bool force_heating = false;

	if (pushbtn_pressed && !digitalRead(pushbtn)) {
		delay(5);
		if (!digitalRead(pushbtn)) {
			force_heating = true;
			pushbtn_pressed = false;
		}
	}

	if (force_heating) {
		change_pwm("100");

		// Force heating for 20 minutes
		forced_heating_until = millis() + 20 * 60 * 1000; 

		// Overflow: ignore
		if (forced_heating_until < millis()) {
			forced_heating_until = 0;
		}
		Serial.println("Forcing heating");
	}

	if (millis() > forced_heating_until) {
		forced_heating_until = 0;
	}

	if (millis() > send_next_ping_at) {
		udp_send("ping");
		send_next_ping_at = millis() + 15L * 60L * 1000L;
	}

	char packetBuffer[255];
	int packetSize = udp.parsePacket();
	if (packetSize) {
		// read the packet into packetBufffer
		int len = udp.read(packetBuffer, 255);
		if (len > 0) {
			packetBuffer[len] = 0;
		}

		parse_cmd(&packetBuffer[0]);
	}

	delay(50); // needed to take advantage of modem sleep (70mA -> 50mA)
}
