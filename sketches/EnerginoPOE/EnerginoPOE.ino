/*
 * Energino
 *
 * Energino POE is a energy consumption meter for DC loads
 *
 * Circuit:
 *  Analog inputs attached to pins A2 (Current), A1 (Voltage)
 *  Digital output attached to pin D4 (Relay)
 *  Green led attached to pin D3
 *  Yellow led attached to pin D2
 *  Red led attached to pin D5
 *
 * Supported commands from the serial:
 *  #P<integer>, sets the period between two updates (in ms) [default is 2000]
 *  #S<0/1>, sets the relay configuration, 0 load on, 1 load off [default is 0]
 *  #A<integer>, sets the value in ohms of the R1 resistor [default is 100000]
 *  #B<integer>, sets the value in ohms of the R2 resistor [default is 10000]
 *  #C<integer>, sets the current sensor offset in mV [default is 2500]
 *  #D<integer>, sets the current sensor sensitivity in mV [default is 185]
 *  #R, reset the configuration to the defaults
 *  #T, self-tune the current sensor offset (use with no load attached)
 *  #Z, print settings
 *  #F<feed>, sets the feed id [default is 0]
 *  #K<key>, sets the Xively authentication key [default is -]
 *  #U<url>, sets the Xively URL [default is https://api.xively.com/v2/feeds/]
 *
 * No updates are sent over the Ethernet interface if the feed id is set to 0
 *
 * Serial putput:
 *   #EnerginoPOE,0,<voltage>,<current>,<power>,<relay>,<period>,<samples>,
 *   <voltage_error>,<current_error>,<feed>,<url>,<key>
 *
 * RESTful interface:
 *   Reading:
 *     GET http://<ipaddress>/arduino/datastreams
 *     GET http://<ipaddress>/arduino/datastreams/voltage
 *     GET http://<ipaddress>/arduino/datastreams/current
 *     GET http://<ipaddress>/arduino/datastreams/power
 *     GET http://<ipaddress>/arduino/datastreams/switch
 *   Switching on:
 *     GET http://<ipaddress>/arduino/datastreams/switch/0
 *   Switching off:
 *     GET http://<ipaddress>/arduino/datastreams/switch/1
 *
 * created 22 August 2014
 * by Roberto Riggio
 *
 * This code is released under the BSD Licence
 *
 */

#include <Bridge.h>
#include <YunServer.h>
#include <YunClient.h>
#include <energino.h>
#include <energinolive.h>
#include <sma.h>
#include <MemoryFree.h>

#define APIKEY        "foo"
#define FEEDID        0
#define FEEDSURL      "https://api.xively.com/v2/feeds/"

#define RELAYPIN      4
#define CURRENTPIN    A2
#define VOLTAGEPIN    A1

#define GREENLED      3
#define YELLOWLED     2
#define REDLED        5

// Energino parameters
int R1 = 390;
int R2 = 100;
int OFFSET = 2500;
int SENSITIVITY = 185;
int PERIOD = 2000;

// magic string
const char MAGIC[] = "EnerginoPOE";
const int REVISION = 1;

// Moving averages
const int SMAPOINTS = 40;
SMA v_sma(SMAPOINTS);
SMA i_sma(SMAPOINTS);

// External AREF in mV
int AREF = 4096;

void reset() {
  strcpy (settings.magic, MAGIC);
  settings.revision = REVISION;
  settings.period = PERIOD;
  settings.r1 = R1;
  settings.r2 = R2;
  settings.offset = OFFSET;
  settings.sensitivity = SENSITIVITY;
  settings.relaypin = RELAYPIN;
  settings.currentpin = CURRENTPIN;
  settings.voltagepin = VOLTAGEPIN;
  strcpy (settings.apikey, APIKEY);
  settings.feedid = FEEDID;
  strcpy (settings.feedsurl, FEEDSURL);
}

// Instantiate a server enabling the the Yun to listen for connected clients.
YunServer server;

void setup() {
  // Set serial port
  Serial.begin(115200);
  // Set external reference
  analogReference(EXTERNAL);
  // Loading setting
  loadSettings();
  if (strcmp(settings.magic, MAGIC) != 0) {
    reset();
    saveSettings();
  }
  // Default on
  pinMode(settings.relaypin, OUTPUT);
  digitalWrite(settings.relaypin, LOW);
  // Init bridge library
  Bridge.begin();
  // listen on port
  server.listenOnLocalhost();
  server.begin();
  // Use the led 13 and GREEN to notify that the
  // setup completed
  pinMode(13, OUTPUT);
  digitalWrite(13, HIGH);
  pinMode(GREENLED, OUTPUT);
  digitalWrite(GREENLED, LOW);
  // Set defaults
  pinMode(YELLOWLED, OUTPUT);
  digitalWrite(YELLOWLED, HIGH);
  pinMode(REDLED, OUTPUT);
  digitalWrite(REDLED, HIGH);
  // Set last update to now
  lastUpdated = millis();
}

void loop() {
  // Make sure that update period is not too high
  // when pushing data to Xively (one sample every
  // 2 seconds should be a reasonable lower boud)
  if ((settings.feedid != 0) && (settings.period < 2000)) {
    settings.period = 2000;
  }
  // Get clients coming from server
  YunClient client = server.accept();
  // There is a new client?
  if (client) {
    // Process request
    process(client);
    // Close connection and free resources.
    client.stop();
  }
  // Parse incoming commands
  serParseCommand(AREF);
  // Instant values are too floating,
  // let's smooth them up
  int v = analogRead(settings.voltagepin);
  int i = analogRead(settings.currentpin);
  v_sma.add(v);
  i_sma.add(i);
  if (lastUpdated + settings.period <= millis()) {
    // Conversion
    VFinal = v_sma.avg();
    IFinal = i_sma.avg();
    lastSamples = SMAPOINTS;
    // dump to serial
    dumpToSerial(AREF);
    // send data to remote host
    if (settings.feedid > 0)
      digitalWrite(YELLOWLED, LOW);
    else
      digitalWrite(YELLOWLED, HIGH);
    sendData(AREF);
    // set last update
    lastUpdated = millis();
  }
}
