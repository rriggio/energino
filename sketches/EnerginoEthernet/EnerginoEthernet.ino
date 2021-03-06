/*
 * EnerginoEthernet
 *
 * This sketch connects an Arduino equipped with the Energino shield and with 
 * an Ethernet shield with a web service implementing the Xively REST API. It 
 * also implements a serial interface.
 *
 * Circuit:
 *  Analog inputs attached to pins A0 (Current), A1 (Voltage)
 *  Digital output attached to pin D3 (Relay)
 *
 * Supported commands from the serial:
 *  #P<integer>, sets the period between two updates (in ms) [default is 2000]
 *  #S<0/1>, sets the relay configuration, 0 load on, 1 load off [default is 0]
 *  #A<integer>, sets the value in ohms of the R1 resistor [default is 100000]
 *  #B<integer>, sets the value in ohms of the R2 resistor [default is 10000]
 *  #C<integer>, sets the offset in mV of the current sensor [default is 2500]
 *  #D<integer>, sets the sensitivity in mV of the current sensor [default is 185]
 *  #R, resest the configuration to the defaults
 *  #F<feed>, sets the feed id [default is 0]
 *  #K<key>, sets the COSM authentication key [default is foo]
 *  #H, sets the remote host ip address [default is 216.52.233.121 (Cosm)]
 *  #O, sets the remote host port [default is 80]
 *
 * No updates are sent over the Ethernet interface if the feed id is set to 0
 *
 * Serial putput:
 *   #Energino,0,<voltage>,<current>,<power>,<relay>,<period>,<samples>,<feed>,<url>,<key>
 *
 * RESTful interface: 
 *  This sketch accepts HTTP requests in the form GET /<cmd>/<param>/[value], where "cmd" 
 *  can be either "read" or "write", param is one of the following parameters:
 *   datastreams [read]
 *   switch [read|write]
 *  Examples: 
 *   GET http://<ipaddress>/read/datastreams
 *   GET http://<ipaddress>/read/switch
 *   GET http://<ipaddress>/write/switch/0
 *   GET http://<ipaddress>/write/switch/1
 *
 * created 31 October 2012
 * by Roberto Riggio
 *
 * This code is released under the BSD Licence
 *
 */

#include <SPI.h>
#include <Ethernet.h>
#include <EEPROM.h>
#include <avr/eeprom.h>

#define APIKEY        "foo"                               // replace your Xively api key here
#define FEEDID        0                                   // replace your feed ID

#define RELAYPIN      2
#define CURRENTPIN    A0
#define VOLTAGEPIN    A1

// Energino parameters
int R1 = 100;
int R2 = 10;
int OFFSET = 2500;
int SENSITIVITY = 185;
int PERIOD = 2000;

// Control loop paramters
long sleep = 0;
long delta = 0;

// Last computed values
float VFinal = 0.0;
float IFinal = 0.0;

// Xively configuration
IPAddress HOST(216,52,233,121);
long PORT = 80;

// Server configuration parameters, energino will listen for
// incoming requests on this port
const long SERVER_PORT = 80;

// working vars
EthernetServer server(SERVER_PORT);

// magic string
const char MAGIC[] = "EnerginoEthernet";
const int REVISION = 1;

// Permanent configuration
struct settings_t {
  char magic[17];
  long period;
  int r1;
  int r2;
  int offset;
  int sensitivity;
  byte mac[6];
  char apikey[49];
  long feedid;
  IPAddress host;
  long port;
} 
settings;

void reset() {
  strcpy (settings.magic,MAGIC);
  settings.period = PERIOD;
  settings.r1 = R1;
  settings.r2 = R2;
  settings.offset = OFFSET;
  settings.sensitivity = SENSITIVITY;
  settings.feedid = FEEDID;
  settings.host = HOST;
  settings.port = PORT;
  strcpy (settings.apikey,APIKEY);  
  settings.mac[0] = 0x90;
  settings.mac[1] = 0xA2;
  settings.mac[2] = 0xDA;
  for (int i = 3; i < 6; i++) {
    randomSeed(analogRead(0));
    settings.mac[i] = random(0, 255);
  }
}

void setup() {
  // Set serial port
  Serial.begin(115200); 
  // Default on
  pinMode(RELAYPIN,OUTPUT);
  digitalWrite(RELAYPIN, LOW);
  // Loading setting 
  eeprom_read_block((void*)&settings, (void*)0, sizeof(settings));
  if (strcmp(settings.magic, MAGIC) != 0) {
    reset();
    eeprom_write_block((const void*)&settings, (void*)0, sizeof(settings));
  }
  // set sleep counter
  resetSleep(settings.period);
  // Try to configure the ethernet using DHCP
  Serial.print("@dhcp... ");
  if (Ethernet.begin(settings.mac) == 0) {
    Serial.println("fail");
    return;
  }
  Serial.println("done");
  // Start server
  server.begin();
}

void loop() {

  // Make sure that update period is not too high
  // when pushing data to Xively (one sample every 
  // 5 seconds should be a reasonable lower boud)
  if ((settings.feedid != 0) && (settings.period < 5000)) {
    resetSleep(5000);
  }

  // Start profiling
  long started = millis();

  // Parse incoming commands
  serParseCommand();

  // Listen for incoming requests
  handleRequests();

  // Instant values are too floating,
  // let's smooth them up
  long VRaw = 0;
  long IRaw = 0;

  for(long i = 0; i < sleep; i++) {
    VRaw += analogRead(A1);
    IRaw += analogRead(A0);
  }

  // Conversion
  VFinal = scaleVoltage((float)VRaw/sleep);
  IFinal = scaleCurrent((float)IRaw/sleep);

  // send data to remote host
  sendData();

  // dump to serial
  dumpToSerial();

  // profiling
  delta = abs(millis() - started);

  // Control loop
  sleep -= 5 * (delta - settings.period);
  if (sleep < 5) {
    sleep = 5;
  }

}

void serParseCommand()
{
  // if serial is not available there is no point in continuing
  if (!Serial.available()) {
    return;
  }
  // working vars
  char cmd = '\0';
  int i, serAva;
  char inputBytes[60] = { 
    '\0'               };
  char * inputBytesPtr = &inputBytes[0];
  // read command from serial
  serAva = Serial.available();
  for (i = 0; i < serAva; i++) {
    char chr = Serial.read();
    if (i == 0) {
      if (chr != '#') {
        return;
      } 
      else {
        continue;
      }
    } 
    else if (i == 1) {
      cmd = chr;
    } 
    else{
      inputBytes[i-2] = chr;
    }
  }
  // null-terminate input buffer
  inputBytes[i] = '\0';
  // execute command
  if (cmd == 'R') {
    reset();
  }
  else if (cmd == 'F') {
    long value = atol(inputBytesPtr);
    if (value >= 0) {
      settings.feedid = value;
    }
  } 
  else if (cmd == 'K') {
    strncpy(settings.apikey, inputBytes,49);
    settings.apikey[48] = '\0';
  }
  else if (cmd == 'H') {
    byte host[4];
    char *p = strtok(inputBytes, ".");  
    for (int i = 0; i < 4; i++) {
      host[i] = atoi(p);
      p = strtok(NULL, ".");
    }
    settings.host=IPAddress(host);
  }  
  else if (cmd == 'O') {
    long value = atol(inputBytesPtr);
    if (value > 0) {
      settings.port = value;
    }
  }   
  else {
    int value = atoi(inputBytesPtr);
    if (value < 0) {
      return;
    }
    if (cmd == 'P') {
      resetSleep(value);
    } 
    else if (cmd == 'A') {
      settings.r1 = value;
    } 
    else if (cmd == 'B') {
      settings.r2 = value;
    } 
    else if (cmd == 'C') {
      settings.offset = value;
    } 
    else if (cmd == 'D') {
      settings.sensitivity = value;
    }
    else if (cmd == 'S') {
      if (value > 0) {
        digitalWrite(RELAYPIN, HIGH);
      } 
      else {
        digitalWrite(RELAYPIN, LOW);
      }
    } 
  }
  eeprom_write_block((const void*)&settings, (void*)0, sizeof(settings)); 
}

// This method accepts HTTP requests in the form GET /<cmd>/<param>/[value]
void handleRequests() {

  EthernetClient request = server.available();

  if (request) {

    String method = request.readStringUntil('/');
    method.trim();

    if (method != "GET") {
      request.print(F("HTTP/1.1 501 Not Implemented\n\n"));
      request.stop();
      return;
    }

    String url = request.readStringUntil(' ');
    url.trim();

    if (url.startsWith("arduino/datastreams")) {

      String command = url.substring(20, -1);

      if (command == "current") {
        sendReply(request,"current",IFinal);
        request.stop();
        return;
      }

      if (command == "voltage") {
        sendReply(request,"voltage",VFinal);
        request.stop();
        return;        
      }

      if (command == "power") {
        sendReply(request,"power",VFinal*IFinal);
        request.stop();
        return;
      }

      if (command == "switch") {
        sendReply(request,"switch",digitalRead(RELAYPIN));
        request.stop();
        return;
      }

      if (command == "switch/0") {
        digitalWrite(RELAYPIN, 0);
        sendReply(request,"switch",digitalRead(RELAYPIN));
        request.stop();
        return;
      }

      if (command == "switch/1") {
        digitalWrite(RELAYPIN, 1);
        sendReply(request,"switch",digitalRead(RELAYPIN));
        request.stop();
        return;
      }

      if (command == "") {
        request.print(F("HTTP/1.1 200 OK\nContent-Type: application/json\n\n"));
        request.print(F("{\"version\":\"1.0.0\","));
        request.print(F("\"datastreams\":["));
        request.print(F("{\"id\":\"voltage\",\"current_value\":"));
        request.print(VFinal);
        request.println(F("},"));
        request.print(F("{\"id\":\"current\",\"current_value\":"));
        request.print(IFinal);
        request.println(F("},"));
        request.print(F("{\"id\":\"power\",\"current_value\":"));
        request.print(VFinal * IFinal);
        request.println(F("},"));
        request.print(F("{\"id\":\"switch\",\"current_value\":"));
        request.print(digitalRead(RELAYPIN));
        request.println(F("}"));
        request.println(F("]"));
        request.println(F("}"));
        request.stop();
        return;
      }
    }

    request.print(F("HTTP/1.1 404 Not Found\n\n"));
    request.stop();
    return;

  }
}

void sendReply(EthernetClient client, String cmd, float value) {
  client.print(F("HTTP/1.1 200 OK\nContent-Type: application/json\n\n"));
  client.print(F("{\"version\":\"1.0.0\","));
  client.print(F("\"id\":\""));
  client.print(cmd);
  client.print(F("\",\"current_value\":"));
  client.print(value);
  client.println(F("}"));
}

// this method makes a HTTP connection to the server
void sendData() {
  // check if the feed has been initialized
  if (settings.feedid == 0) {
    return;
  }
  // try to connect
  EthernetClient client;    
  int ret = client.connect(settings.host, settings.port);
  if (ret == 1) {
    client.print("PUT /v2/feeds/");
    client.print(settings.feedid);
    client.println(".csv HTTP/1.1");
    client.println("Host: api.xively.com");
    client.print("X-ApiKey: ");
    client.println(settings.apikey);
    client.println("Content-Type: text/csv");
    client.println("Content-Length: 53");
    client.println();

    char buffer[10]; 

    dtostrf(IFinal, 2, 3, buffer);
    client.print("current,");
    client.println(buffer);

    dtostrf(VFinal, 2, 3, buffer);
    client.print("voltage,");
    client.println(buffer);

    dtostrf(VFinal*IFinal, 2, 3, buffer);
    client.print("power,");
    client.println(buffer);

    client.print("switch,");
    client.println(digitalRead(RELAYPIN));

    client.flush();
    client.stop();
  }
}

void dumpToSerial() {
  // Print data also on the serial
  Serial.print("#");
  Serial.print(MAGIC);
  Serial.print(",");
  Serial.print(REVISION);
  Serial.print(",");
  Serial.print(VFinal,3);
  Serial.print(",");
  Serial.print(IFinal,3);
  Serial.print(",");
  Serial.print(VFinal * IFinal,3);
  Serial.print(",");
  Serial.print(digitalRead(RELAYPIN));
  Serial.print(",");
  Serial.print(delta);
  Serial.print(",");
  Serial.print(sleep);
  Serial.print(",");
  Serial.print(settings.feedid);
  Serial.print(",");
  Serial.print("http://");
  for (byte thisByte = 0; thisByte < 4; thisByte++) {
    // print the value of each byte of the IP address:
    Serial.print(settings.host[thisByte], DEC);
    if (thisByte < 3) {
      Serial.print(".");
    }
  }
  Serial.print(":");
  Serial.print(settings.port);  
  Serial.print("/v2/feeds/");
  Serial.print(",");
  Serial.println(settings.apikey);
}

float scaleVoltage(float voltage) {
  float tmp = ( voltage * 5 * (settings.r1 + settings.r2)) / ( settings.r2 * 1024 );
  return (tmp > 0) ? tmp : 0.0;
}

float scaleCurrent(float current) {
  float tmp = ( current * 5000 / 1024  - settings.offset) / settings.sensitivity;
  return (tmp > 0) ? tmp : 0.0;
}

void resetSleep(long value) {
  sleep = (value * 4400) / 1000;
  settings.period = value;
}


