#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRtimer.h>
#include <IRutils.h>

#include <DHTesp.h>

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

/*
 Haier Home AC remote MQTT controller firmware
*/

/* Known protocol details
   Pulses 1,2,3 - 2850..3100
   Pulse 4 - 4300..4350
   Marks - 500
   Spaces used to transmit data:
       Short(0) - 600..850
       Long(1) - 1700..1950


   Full Raw Data packet length - 149 pulses. Pulses 5-150 encode the data packet
   Full Data Packet length - 9 bytes / 72 bits / 144 pulses

   Last byte is CRC - SUM of previous 8 bytes modulo 0x100
*/

// raw decoder data
#define DATA_0_LOW  500
#define DATA_0_HIGH  800
#define DATA_1_LOW  1600
#define DATA_1_HIGH  1950

// initial AC remote command - OFF 21 COOL health ON sl,tm,sw OFF
const byte init_command[72] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1};

// Haier protocol commands
// CMD - change mode commands
const byte cmd[11] = { B0000, B0001, B0010, B0011, B0110, B0111, B1000, B1100, B1101, B1001, B1010 } ;
//                     OFF    ON     Mode   Fan    TmpUP  TmpDN  Sleep  Swing  Hlth   tSET   tOFF
//                     0      1      2      3      4      5      6      7      8      9      10

// Network settings
const char* ssid = "";
const char* password = "";
const char* mqtt_server = ""; // server ip

// MQTT settings
// global MQTT Context home/bedroom/ac/
// Parameters to be set: POWER, MODE, FAN, TEMP, SWING, HEALTH. SLEEP [TIMER maybe later]
// Additional params to publish: DHT11 actual temp and humidity, AC power state
// Topics to Subscribe end with set
// LWT
const char* topicLWT = "home/bedroom/ac/status";
const char* msgLWT = "Offline";

WiFiClient espClient;
PubSubClient client(espClient);
IPAddress ip;
char* ipStr;

// Wemos D1 Mini - Builtin led is connected to D4/GPIO2
#define BUILTIN_LED 2

//IR settings
#define BAUD_RATE 115200
#define CAPTURE_BUFFER_SIZE 1024
#define TIMEOUT 50U
#define MIN_UNKNOWN_SIZE 120

IRrecv irrecv(D2, CAPTURE_BUFFER_SIZE, TIMEOUT, true);
decode_results results;

IRsend irsend(D8); //an IR led is connected to pin D8 (not D4) on NodeMCU

//DHT sensor setup
DHTesp dht;

long lastMsg = 0;
char msg[50];
int value = 0;

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  WiFi.disconnect();
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  ip = WiFi.localIP();
  Serial.println(ip);
}

// IR commands send here
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  char buf[10];
  snprintf(buf,length + 1, "%s", (char*)payload);
  String cmdData = String(buf);
  //parse callback data
  if (strstr(topic, "power") != NULL) { //ON, OFF
    Serial.print("POWER CMD: ");
    Serial.println(buf);
    if (cmdData == "ON") setPOWER(1);
    else setPOWER(0);

  } else if (strstr(topic, "mode") != NULL) { // AUTO, COOL, DRY, HEAT, FAN
    Serial.print("MODE CMD: ");
    Serial.println(buf);
    if (strcmp(buf, "AUTO") == 0) setMODE(0);
    else if (strcmp(buf, "COOL") == 0) setMODE(1);
    else if (strcmp(buf, "DRY") == 0) setMODE(2);
    else if (strcmp(buf, "HEAT") == 0) setMODE(3);
    else if (strcmp(buf, "FAN") == 0) setMODE(4);
    else if (strcmp(buf, "OFF") == 0) setPOWER(0);

  } else if (strstr(topic, "fan") != NULL) { // AUTO, HIGH, MEDIUM, LOW
    Serial.print("FAN CMD: ");
    Serial.println(buf);
    if (strcmp(buf, "AUTO") == 0) setFAN(4);
    else if (strcmp(buf, "LOW") == 0) setFAN(1);
    else if (strcmp(buf, "MEDIUM") == 0) setFAN(2);
    else if (strcmp(buf, "HIGH") == 0) setFAN(3);

  } else if (strstr(topic, "temp") != NULL) { // numeric value, 16..30 C
    // take two digits, HASS passes float, we don't need it
    char tempData[3];
    snprintf(tempData, 3, "%s", buf);
    Serial.print("TEMP Target: ");
    Serial.println(tempData);
    setTEMP(atoi(tempData));

  } else if (strstr(topic, "swing") != NULL) { // OFF, UP, DOWN, SWING
    Serial.print("SWING CMD: ");
    Serial.println(buf);
    if (strcmp(buf, "OFF") == 0) setSWING(0);
    else if (strcmp(buf, "UP") == 0) setSWING(1);
    else if (strcmp(buf, "DOWN") == 0) setSWING(2);
    else if (strcmp(buf, "SWING") == 0) setSWING(3);

  } else if (strstr(topic, "health") != NULL) { // ON, OFF
    Serial.print("HEALTH CMD: ");
    Serial.println(buf);
    if (strcmp(buf, "ON") == 0) setHEALTH(1);
    else setHEALTH(0);

  } else if (strstr(topic, "sleep") != NULL) { // ON, OFF
    Serial.print("SLEEP CMD: ");
    Serial.println(buf);
    if (strcmp(buf, "ON") == 0) setSLEEP(1);
    else setSLEEP(0);
  } else if (strstr(topic, "sync") != NULL) { // any data to re-publish all states
    Serial.print("SYNC CMD.");
    //Serial.println(buf);
    pubTopics();
  }
  crcRecalc();
  sendCode();
  client.publish("home/bedroom/ac/power", getPOWER(), true);
}

// publish data at start to all topics
void pubTopics() {
  // controller status
  client.publish("home/bedroom/ac/status", "Online");
  client.publish("home/bedroom/ac/status/ip", ip.toString().c_str(), true);
  // AC data
  client.publish("home/bedroom/ac/power", getPOWER(), true); //POWER
  client.publish("home/bedroom/ac/mode", getMODE(), true); //MODE
  client.publish("home/bedroom/ac/fan", getFAN(), true);  //FAN
  sprintf(msg,"%i", getTEMP()); client.publish("home/bedroom/ac/temp", msg, true); //TEMP
  client.publish("home/bedroom/ac/swing", getSWING(), true); //SWING
  client.publish("home/bedroom/ac/health", getHEALTH(), true); //HEALTH
  client.publish("home/bedroom/ac/sleep", getSLEEP(), true); //SLEEP
  //Climate data
    //delay(dht.getMinimumSamplingPeriod());
  const char* dhtStatus = dht.getStatusString();
  float humidity = dht.getHumidity();
  float temperature = dht.getTemperature();
  float heatIndex = dht.computeHeatIndex(temperature, humidity, false);
  sprintf(msg,"%i", (int)temperature); client.publish("home/bedroom/climate/temperature", msg);
  sprintf(msg,"%i", (int)humidity); client.publish("home/bedroom/climate/humidity", msg);
  client.publish("home/bedroom/climate/status", dht.getStatusString());
  client.publish("home/bedroom/ac/status", "Online");
}

bool reconnect() {
  // Loop until we're reconnected
  int errorCount = 0;
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(), "homeassistant", "letmeinkkk", topicLWT, 1, 0, msgLWT)) {
      Serial.println("connected");

      // Once connected, publish an announcement...
      pubTopics();
      // ... and resubscribe
      //subscribe data "home/bedroom/ac/+/set", QoS 1
      client.subscribe("home/bedroom/ac/+/set", 1);
      // ... and publish current IP
      client.publish("home/bedroom/ac/status/ip", ip.toString().c_str(), true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      //break after 10 unsuccessful connections
      if (errorCount > 10) {
        Serial.println("MQTT error limit reached, restarting WiFi");
        return false;
      }
      else ++errorCount;
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying and blink LED
      digitalWrite(BUILTIN_LED, LOW);
      delay(2500);
      digitalWrite(BUILTIN_LED, HIGH);
      delay(2500);
    }
  }
  return true;
}

// haier protocol implementation

// Storage for the recorded code

uint16_t rawCodes[149]; // The raw durations
byte haierCode[72]; // bit code, equal to init_code at start
int codeLen; // The length of the code

// decode Haier Data Packet and set as current code
// so, if AC remote is used, we have states synchronised
void decodeHaier(decode_results *results) {
  int count = results->rawlen;
  int rawDataBit;
  byte tmpCode[72]; // temp storage for processed code
  bool errorsFound = false; // check if errors found in raw pulse data
  codeLen = results->rawlen - 1;
  // To store raw codes:
  // Drop first value (gap)
  // Convert from ticks to microseconds
  // Tweak marks shorter, and spaces longer to cancel out IR receiver distortion
  // skip marks processing
  // for debug
  /*
  for (int i = 0; i <= codeLen; i++) {
    Serial.print(results->rawbuf[i] * RAWTICK);
    Serial.print(", ");
  }
  Serial.println("");
  */
  // get raw bit data
  for (int i = 6; i <= codeLen; i = i + 2) {
    // get data bit value
    rawDataBit = results->rawbuf[i] * RAWTICK;
    // decode bit value
    if ( rawDataBit >= DATA_0_LOW && rawDataBit <= DATA_0_HIGH) {
      tmpCode[(i - 6) / 2] = 0;
    } else if ( rawDataBit >= DATA_1_LOW && rawDataBit <= DATA_1_HIGH) {
      tmpCode[(i - 6) / 2] = 1;
    } else {
      //rawCodes[i - 6] = rawDataBit;
      errorsFound = true;
      break;
    }
    //Serial.print(rawCodes[i-6], DEC);
    //Serial.print(",");
  }
  if (errorsFound) Serial.print("Some pulses not recognised, no packet data available");
  else { // copy code to current code
    Serial.println("Bit code:");
    for (int i = 0; i < 72; i++) {
      haierCode[i] = tmpCode[i];
      Serial.print(haierCode[i]);
      Serial.print(",");
    }
    pubTopics(); // if state changed from original remote - keep MQTT in sync
  }
  Serial.println("");
  errorsFound = false;
}

// encode and send haierCode
void sendCode() {
  int idx;
  codeLen = 149;
  // prefix
  rawCodes[0] = 3050;
  rawCodes[1] = 3150;
  rawCodes[2] = 3050;
  rawCodes[3] = 4400;

  //bits -> durations
  for (int i = 4; i < 150; i = i + 2) {
    idx = (i - 4) / 2;
    rawCodes[i] = 560; //mark
    //space (data)
    if (haierCode[idx] == 0) rawCodes[i + 1] = 750;
    else rawCodes[i + 1] = 1950;
  }
  // disable receiver to afoid feedback
  irrecv.disableIRIn();
  // Assume 38 KHz
  irsend.sendRaw(rawCodes, 149, 38);
  Serial.println("Sent raw");
  // re-enable receiver
  irrecv.enableIRIn();
}

// haierCode bits rewrite
// set -len- bits of -value- to haierCode, starting from -offset-
void setHaierBits(byte offset, byte len, byte value) {
  for (byte i = 0; i < len; i++) {
    haierCode[offset + i] = bitRead(value, len - 1 - i);
    Serial.print(haierCode[offset + i]);
  }
  Serial.println("");
}

//get -len- bits of current haierCode, starting from -offset- and return in byte
byte getHaierBits(byte offset, byte len) {
  byte result = 0;
  for (byte i = 0; i < len; i++) bitWrite(result, len - 1 - i, haierCode[offset + i]);
  return result;
}

// recalc and set CRC of haierCode
// bytes 64..71
void crcRecalc() {
  byte haierByteCode[8];
  byte crc = 0;
  // convert bits into bytes
  for (byte i = 0; i < 8; i++) {
    for (byte k = 0; k < 8;  k++) {
      if (haierCode[8 * i + k] == 0) bitClear(haierByteCode[i], 7 - k);
      else bitSet(haierByteCode[i], 7 - k);
      //Serial.print(haierCode[8*i+k]);
      //Serial.print(",");
    }
  }
  //Serial.println("");
  //Byte Code
  Serial.print("Byte code : ");
  for (byte i = 0; i < 8; i++) {
    Serial.print(haierByteCode[i], HEX);
    Serial.print(",");
  }
  Serial.println("");

  //calculate CRC
  for (byte i = 0; i < 8; i++) crc += haierByteCode[i];
  Serial.print("CRC: ");
  Serial.println(crc, HEX);
  //set CRC bits in haierCode
  for (byte i = 0; i < 8; i++) haierCode[64 + i] = bitRead(crc, 7 - i);
}

// set command bits 12-15
// this called with every other command, so no need to CRC recalc here
void setCMD(byte command) {
  Serial.print("CMD: ");
  setHaierBits(12, 4, cmd[command]);
}

// set power ON/OFF
void setPOWER(byte mode) {
  Serial.print("Power: ");
  Serial.println(mode);
  setCMD(mode); //0-off 1-on
  client.publish("home/bedroom/ac/power", getPOWER(), true);
}

//get POWER state and convert to publish value
char* getPOWER() {
  char* result;
  if (getHaierBits(12,4) == 0) result = "OFF";
  else result = "ON";
  return result;
}

// set temp
// bits 8-11, value=target_temp-16
// there are two Temp set CMDs, UP and DOWN, dont understand sense yet, all settings work with one
void setTEMP(byte target) {
  if (target < 16) target = 16;
  else if (target > 30) target = 30;
  //set command
  setCMD(4);
  // set temp bits
  Serial.print("Temp: ");
  setHaierBits(8, 4, target - 16);
  sprintf(msg,"%i", getTEMP()); client.publish("home/bedroom/ac/temp", msg, true);
}

//get TEMP state and convert to publish value
int getTEMP() {
  int result;
  result = int(getHaierBits(8,4)) + 16;
  return result;
}

//set swing
// bits 16-17 0-Off(Auto), 1-Up, 2-Down, 3-Rotation
void setSWING(byte mode) {
  if ((mode < 0) || (mode > 3)) mode = 0;
  //set command
  setCMD(7);
  // set swing bits
  Serial.print("Swing: ");
  setHaierBits(16, 2, mode);
  client.publish("home/bedroom/ac/swing", getSWING(), true);
}

//get swing state
char* getSWING() {
  char* result;
  switch (getHaierBits(16,2)) {
    case 0:
      result = "OFF";
      break;
    case 1:
      result = "UP";
      break;
    case 2:
      result = "DOWN";
      break;
    case 3:
      result = "SWING";
      break;
  }
  return result;
}

//set health, ion generator control
//bit 34 0-OFF 1-ON
void setHEALTH(byte mode) {
  if ((mode < 0) || (mode > 1)) mode = 0;
  //set command
  setCMD(8);
  // set health bit
  Serial.print("Health: ");
  setHaierBits(34, 1, mode);
  client.publish("home/bedroom/ac/health", getHEALTH(), true);
}

//get HEALTH state and convert to publish value
char* getHEALTH() {
  char* result;
  if (getHaierBits(34,1) == 0) result = "OFF";
  else result = "ON";
  return result;
}

//set sleep
//bit 57, 0-OFF 1-ON
void setSLEEP(byte mode) {
  if ((mode < 0) || (mode > 1)) mode = 0;
  //set command
  setCMD(6);
  // set sleep bit
  Serial.print("Sleep: ");
  setHaierBits(57, 1, mode);
  client.publish("home/bedroom/ac/sleep", getSLEEP(), true);
}

//get SLEEP state and convert to publish value
char* getSLEEP() {
  char* result;
  if (getHaierBits(57,1) == 0) result = "OFF";
  else result = "ON";
  return result;
}

//set FAN
//bits 40-41, 4-Auto speed 1,2,3 = 3,2,1
void setFAN(byte mode) {
  Serial.print("FAN mode "); Serial.println(mode);
  if ((mode < 0) || (mode > 3)) mode = 0;
  //set command
  setCMD(3);
  // set swing bits
  Serial.print("Fan: ");
  setHaierBits(40, 2, 4 - mode);
  client.publish("home/bedroom/ac/fan", getFAN(), true);
}

//get fan state
char* getFAN() {
  char* result;
  switch (getHaierBits(40,2)) {
    case 0:
      result = "AUTO";
      break;
    case 1:
      result = "HIGH";
      break;
    case 2:
      result = "MEDIUM";
      break;
    case 3:
      result = "LOW";
      break;
  }
  return result;
}

//set MODE
//bits 48-50, 0-Auto 1-Cool 2-Dry 3-Heat 4-Fan
void setMODE(byte mode) {
  if ((mode < 0) || (mode > 4)) mode = 0;
  //set command
  setCMD(2);
  // set mode bits
  Serial.print("Mode: ");
  setHaierBits(48, 3, mode);
  client.publish("home/bedroom/ac/mode", getMODE(), true);
}

//get MODE state
char* getMODE() {
  char* result;
  switch (getHaierBits(48,3)) {
    case 0:
      result = "AUTO";
      break;
    case 1:
      result = "COOL";
      break;
    case 2:
      result = "DRY";
      break;
    case 3:
      result = "HEAT";
      break;
    case 4:
      result = "FAN";
      break;
  }
  return result;
}

//Too lazy to implement timer and clock control commands now )))


void setup() {
  pinMode(BUILTIN_LED, OUTPUT);     // Initialize the BUILTIN_LED pin as an output
  Serial.begin(115200);
  //Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(200);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  irsend.begin();
  irrecv.enableIRIn(); // Start the IR receiver
  dht.setup(D1); //start DHT
  // bit code init
  for (byte i = 0; i < 72; i++) {
    haierCode[i] = init_command[i];
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(BUILTIN_LED, LOW); // LED ON while no WiFi connection
    setup_wifi();
    digitalWrite(BUILTIN_LED, HIGH);
  }
  else {
    //check MQTT
    if (!client.connected()) {
      if (!reconnect()) ESP.reset(); //restart if 10 MQTT reconnects unsuccessful;
    } else {
      client.loop(); // process MQTT

      //check IR receiver
      if (irrecv.decode(&results)) {
        Serial.println("IR code received.");
        decodeHaier(&results);
        irrecv.resume(); // Receive the next value
      }
      delay(100);

      // blink and report status
      long now = millis();
      int period = 20; // seconds of status renew
      if (now - lastMsg > period * 1000) { // every 20 seconds in production, 2 sec for debug
        // short LED blink
        digitalWrite(BUILTIN_LED, LOW);
        delay(50);
        digitalWrite(BUILTIN_LED, HIGH);
        //uptime report
        lastMsg = now;
        ++value;
        snprintf (msg, 75, "%ld", value * period);
        Serial.print("Keepalive seconds: ");
        Serial.println(msg);
        client.publish("home/bedroom/ac/status/counter", msg);
        //status report
        pubTopics();
      }
    }
  }
  yield(); //feed WDT
}
