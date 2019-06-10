//DELFA remote controller

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRtimer.h>
#include <IRutils.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <RCSwitch.h>
#include <EEPROM.h>

#define DECODE_COOLIX true
#define SEND_COOLIX true

//constant commands
#define cmdOFF    0xB27BE0
#define cmdSLEEP  0xB2E003
#define cmdTURBO  0xB5F5A2
#define cmdSWING  0xB26BE0
#define cmdDIRECT 0xB20FE0
#define cmdLED    0xB5F5A5

//modes
#define modeAUTO  0x8
#define cmdCOOL  0x0
#define modeDRY   0x4
#define modeHEAT  0xC
#define modeFAN   0xE4 // last byte always E4 in FAN mode

//fan speeds
#define fanLOW    0x9
#define fanMEDIUM 0x5
#define fanHIGH   0x3
#define fanAUTO   0xB

//433MHz remote commands
#define radioSTREEToff  12583728 //B
#define radioSTREETon   12583872 //A
#define radioGARDENoff  12583683 //C
#define radioGARDENon   12583692 //D

/* Status LED - D4
 * DHT - D1
 * IR receiver - D2
 * IR LED - D8
 * 433MHZ Receive - D7
 * 433MHz send - D6
*/
#define BUILTIN_LED D4
#define IR_LED D8
#define DHT_PIN D1
#define RADIO_SEND_PIN D6
#define RADIO_RCV_PIN D7

const byte temperature[14] = {0,1,3,2,6,7,5,4,0xC,0xD,9,8,0xA,0xB}; //temperatures from 17 to 30

uint32_t delfaCMD = 0x00B2BF60; //default settings COOL 21C FAN AUTO, stores last active CMD with settings

//433 switch state
uint8_t switchState = 0; // 00 - all off, 01,10,11 - channels state, lower bit - street light, higher bit - garden light

// IR Receiver settings
uint16_t RECV_PIN = D2;
IRrecv irrecv(RECV_PIN);
decode_results results;

//IR LED
IRsend irsend(IR_LED);

//DHT sensor setup
DHTesp dht;

//433MHz settings
RCSwitch radioSwitch = RCSwitch();

// state of AC options
struct acState {
  bool power; // ON or OFF
  char mode[7]; // AUTO, COOL, DRY, HEAT, FAN
  unsigned int temperature; //17-30 C
  char fan[7]; // AUTO, LOW, MEDIUM, HIGH
  bool sleep = false;
  //bool swing; //swing
  //bool led; //led panel light
};

acState delfaState;

// Network settings
const char* ssid = "";
const char* password = "";
const char* mqttLogin = "";
const char* mqttPwd = "";

const char* mqtt_server = ""; // server IP

// MQTT settings
// global MQTT Context garden/house/top/ac
// Parameters to be set: POWER, MODE, FAN, TEMP, SWING, HEALTH. SLEEP [TIMER maybe later]
// Additional params to publish: DHT11 actual temp and humidity, AC power state
// Topics to Subscribe end with set
// LWT
const char* topicLWT = "garden/house/top/ac/status";
const char* msgLWT = "Offline";

WiFiClient espClient;
PubSubClient client(espClient);
IPAddress ip;
char* ipStr;

long lastMsg = 0;
char msg[50];
int value = 0;

char buf[10];

//WiFi init
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

//MQTT callback function
// IR commands send here
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  snprintf(buf,length + 1, "%s", (char*)payload);
  uint32_t activeCMD = 0;
  //parse callback data
  if (strstr(topic, "power") != NULL) { //ON, OFF
    Serial.print("POWER CMD: ");
    Serial.println(buf);
    if ( strcmp(buf, "ON") != 0) delfaState.power = false;
    else {
      delfaState.power = true;
      delfaState.sleep = false;
    }
    activeCMD = encodeCMD();
  } else if (strstr(topic, "mode") != NULL) { // AUTO, COOL, DRY, HEAT, FAN, QHEAT, QCOOL, LED, SWING
    Serial.print("MODE CMD: "); Serial.println(buf);
    if ( (strcmp(buf, "AUTO") == 0 ) ||
         (strcmp(buf, "COOL") == 0 ) ||
         (strcmp(buf, "DRY") == 0 ) ||
         (strcmp(buf, "HEAT") == 0 ) ||
         (strcmp(buf, "FAN") == 0 ) )
        {
      delfaState.power = true;
      delfaState.sleep = false;
      sprintf(delfaState.mode, "%s", buf);
      activeCMD = encodeCMD();
    } else if ( strcmp(buf, "QHEAT") == 0 ) { //quick heat: 21C fan HIGH
        delfaState.power = true;
        delfaState.sleep = false;
        sprintf(delfaState.mode, "%s", "HEAT");
        delfaState.temperature = 21;
        sprintf(delfaState.fan, "%s", "HIGH");
        activeCMD = encodeCMD();
    } else if ( strcmp(buf, "QCOOL") == 0 ) { //quick cool: 23C fan HIGH
        delfaState.power = true;
        delfaState.sleep = false;
        sprintf(delfaState.mode, "%s", "COOL");
        delfaState.temperature = 23;
        sprintf(delfaState.fan, "%s", "HIGH");
        activeCMD = encodeCMD();
    } else if ( strcmp(buf, "SWING") == 0 ) {
        if (delfaState.power == true) activeCMD = cmdSWING; // SWING toggle
        else Serial.println("Power OFF, SWING cmd ignored");
    }
      else if ( strcmp(buf, "LED") == 0 ) activeCMD = cmdLED; // LED toggle

  } else if (strstr(topic, "fan") != NULL) { // AUTO, LOW, MEDIUM, HIGH
    Serial.print("FAN CMD: ");
    Serial.println(buf);
    if ( (strcmp(buf, "AUTO") == 0 ) ||
         (strcmp(buf, "LOW") == 0 ) ||
         (strcmp(buf, "MEDIUM") == 0 ) ||
         (strcmp(buf, "HIGH") == 0 )) {
      delfaState.power = true;
      delfaState.sleep = false;
      sprintf(delfaState.fan, "%s", buf);
      activeCMD = encodeCMD();
    }
  } else if (strstr(topic, "sleep") != NULL) { // AUTO, LOW, MEDIUM, HIGH
      Serial.print("SLEEP CMD: ");
      Serial.println(buf);
      if ((delfaState.power == true) && (strcmp(delfaState.mode, "DRY") != 0) && (strcmp(delfaState.mode, "FAN") != 0)) {
        if ((strcmp(buf, "ON") == 0)) {
          delfaState.sleep = true;
          activeCMD = cmdSLEEP;
        } else if ((strcmp(buf, "OFF") == 0)) {
            delfaState.sleep = false;
            activeCMD = encodeCMD();
        }
      } else Serial.println("Power OFF or mode DRY or FAN, SLEEP cmd ignored");
    } else if (strstr(topic, "temp") != NULL) { // 17-30 C
        Serial.print("TEMP CMD: ");
        Serial.println(buf);
        if (strcmp(delfaState.mode, "FAN") != 0) {
          int tempTarget = 17;
          tempTarget = atoi(buf);
          if (tempTarget < 17) tempTarget = 17;
          else if (tempTarget > 30) tempTarget = 30;
          delfaState.power = true;
          delfaState.sleep = false;
          delfaState.temperature = tempTarget;
          activeCMD = encodeCMD();
        } else Serial.println("FAN mode active, TEMP cmd ignored");
    } else if (strstr(topic, "streetlight") != NULL) { //street light
        Serial.print("Street Light: ");
        Serial.println(buf);
        if ((strcmp(buf, "ON") == 0)) { //Street light on
            bitSet(switchState, 0);
            client.publish("garden/street/streetlight", "ON", true);
            radioSwitch.send(radioSTREETon, 24);
        } else if ((strcmp(buf, "OFF") == 0)) {
            bitClear(switchState, 0);
            client.publish("garden/street/streetlight", "OFF", true);
            radioSwitch.send(radioSTREEToff, 24);
        } else
            Serial.println("unknown command, ignored.");
    } else if (strstr(topic, "gardenlight") != NULL) { //street light
        Serial.print("Garden Light: ");
        Serial.println(buf);
        if ((strcmp(buf, "ON") == 0)) { //Street light on
            bitSet(switchState, 1);
            client.publish("garden/street/gardenlight", "ON", true);
            radioSwitch.send(radioGARDENon, 24);
        } else if ((strcmp(buf, "OFF") == 0)) {
            bitClear(switchState, 1);
            client.publish("garden/street/gardenlight", "OFF", true);
            radioSwitch.send(radioGARDENoff, 24);
        } else
            Serial.println("unknown Radio command, ignored.");
    }
  // store switch state, if changed
  if (EEPROM.read(0) != switchState) {
    EEPROM.write(0, switchState);
    EEPROM.commit();
  }
  // send IR
  if (activeCMD != 0) {
    irrecv.disableIRIn(); //stop receiver
    irsend.sendCOOLIX(activeCMD, COOLIX_BITS, 1);
    irrecv.enableIRIn(); //start receiver again
    Serial.print("Sent code: ");
    Serial.println(activeCMD, HEX);
    pubTopics(); //notify MQTT
  } else Serial.println("Not IR command.");
}

// MQTT connection handle
bool reconnect() {
  // Loop until we're reconnected
  int errorCount = 0;
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(), mqttLogin, mqttPwd, topicLWT, 1, 0, msgLWT)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      pubTopics();
      // ... and resubscribe
      //subscribe data "garden/house/top/ac/+/set", QoS 1
      client.subscribe("garden/house/top/ac/+/set", 1);
      //433 switch command topics subscribe
      client.subscribe("garden/street/streetlight/set", 1);
      client.subscribe("garden/street/gardenlight/set", 1);
      // ... and publish current IP
      client.publish("garden/house/top/ac/status/ip", ip.toString().c_str(), true);
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

// publish AC state to MQTT
void pubTopics() {
  // notify MQTT server
  client.publish("garden/house/top/ac/status", "Online");
  // AC data
  if (delfaState.power == true) sprintf(buf, "%s", "ON"); else sprintf(buf, "%s", "OFF");
    client.publish("garden/house/top/ac/power", buf, true); //POWER
  client.publish("garden/house/top/ac/mode", delfaState.mode, true); //MODE
  client.publish("garden/house/top/ac/fan", delfaState.fan, true);  //FAN
  sprintf(buf,"%i", delfaState.temperature); client.publish("garden/house/top/ac/temp", buf, true); //TEMP
  if (delfaState.sleep == true) sprintf(buf, "%s", "ON"); else sprintf(buf, "%s", "OFF");
    client.publish("garden/house/top/ac/sleep", buf, true); //SLEEP
  if (bitRead(switchState, 0) == 1) sprintf (buf, "%s", "ON"); else sprintf (buf, "%s", "OFF");
    client.publish("garden/street/streetlight", buf, true); //street light
  if (bitRead(switchState, 1) == 1) sprintf (buf, "%s", "ON"); else sprintf (buf, "%s", "OFF");
    client.publish("garden/street/gardenlight", buf, true); //Garden light
  client.loop(); // process MQTT
}


//decode received Delfa command and publish to MQTT server
void decodeDelfa(uint32_t cmd) {
  //parse cmd
  Serial.print("CMD: ");
  switch (cmd) { //first detect constant CMDs or parse others
    case cmdOFF:
      Serial.println("OFF");
      delfaState.power = false;
      delfaState.sleep = false;
      break;
    case cmdSLEEP:
      Serial.println("SLEEP");
      delfaState.sleep = true;
      break;
    case cmdTURBO:
      Serial.println("TURBO");
      //do nothng, this does not work on my AC model
      break;
    case cmdSWING:
      Serial.println("SWING"); //toggle cmd
      break;
    case cmdDIRECT:
      Serial.println("DIRECT");
      break;
    case cmdLED:
      Serial.println("LED"); //toggle cmd
      break;
    default: //parse the command
      if (((cmd & 0xFF0000) >> 16) != 0xB2) { //check prefix
        Serial.println("Wrong CMD prefix"); //do nothing
      } else if ((cmd & 0xFF) == 0xE4) { //check FAN mode
          Serial.print("MODE FAN: ");
          char* fanMode = getFAN(cmd);
          if ( strcmp(fanMode, "unknown") != 0) {
            Serial.println(fanMode);
            sprintf(delfaState.mode, "%s", "FAN");
            sprintf(delfaState.fan, "%s", fanMode);
            delfaState.power = true;
            delfaState.sleep = false;
          }
          else Serial.println("unknown");
      } else if ((cmd & 0xF) == 8) { //check AUTO
          Serial.print("MODE AUTO, TEMP = ");
          unsigned int tempMode = getTEMP(cmd);
          if (tempMode != 0) {
            Serial.println(tempMode);
            sprintf(delfaState.mode, "%s", "AUTO");
            delfaState.temperature = tempMode;
            //fan is always AUTO in AUTO mode
            delfaState.power = true;
            delfaState.sleep = false;
          }
          else Serial.println("unknown");
      } else if ((cmd & 0xF) == 4) { //check DRY
          Serial.print("MODE DRY, TEMP = ");
          unsigned int tempMode = getTEMP(cmd);
          if (tempMode != 0) {
            Serial.println(tempMode);
            sprintf(delfaState.mode, "%s", "DRY");
            delfaState.temperature = tempMode;
            //fan is always AUTO in DRY mode
            delfaState.power = true;
            delfaState.sleep = false;
          }
          else Serial.println("unknown");
      } else if ((cmd & 0xF) == 0) { //check COOL
          Serial.print("MODE COOL, TEMP = ");
          unsigned int tempMode = getTEMP(cmd);
          char* fanMode = getFAN(cmd);
          if ((tempMode != 0) && (fanMode != "")) {
            Serial.print(tempMode);
            Serial.print(", FAN: ");
            Serial.println(fanMode);
            sprintf(delfaState.mode, "%s", "COOL");
            delfaState.temperature = tempMode;
            sprintf(delfaState.fan, "%s", fanMode);
            delfaState.power = true;
            delfaState.sleep = false;
          }
          else Serial.print("unknown");
      } else if ((cmd & 0xF) == 0xC) { //check HEAT
          Serial.print("MODE HEAT, TEMP = ");
          unsigned int tempMode = getTEMP(cmd);
          char* fanMode = getFAN(cmd);
          if ((tempMode != 0) && (fanMode != "")) {
            Serial.print(tempMode);
            Serial.print(", FAN: ");
            Serial.println(fanMode);
            sprintf(delfaState.mode, "%s", "HEAT");
            delfaState.temperature = tempMode;
            sprintf(delfaState.fan, "%s", fanMode);
            delfaState.power = true;
            delfaState.sleep = false;
          }
          else Serial.print("unknown");
      } else { //all other not implemented
        Serial.print("Unsupported CMD: ");
        Serial.println(cmd, HEX);
      }
      Serial.println(cmd, HEX);
      break;
  }
  pubTopics();
}

//decode fan setting
char* getFAN(uint32_t cmd) {
  byte fanDecode = (cmd & 0xF000) >> 12;
  char* result = "unknown";
  switch (fanDecode) {
    case fanLOW:
      sprintf(result, "%s", "LOW");
      break;
    case fanMEDIUM:
      sprintf(result, "%s", "MEDIUM");
      break;
    case fanHIGH:
      sprintf(result, "%s", "HIGH");
      break;
    case fanAUTO:
      sprintf(result, "%s", "AUTO");
      break;
  }
  return result;
}

//encode FAN bits from delfaSate
uint16_t encodeFAN () {
  uint16_t result = 0;
  if ( strcmp(delfaState.fan, "LOW") == 0 ) result = fanLOW;
  else if ( strcmp(delfaState.fan, "MEDIUM") == 0 ) result = fanMEDIUM;
  else if ( strcmp(delfaState.fan, "HIGH") == 0 ) result = fanHIGH;
  else if ( strcmp(delfaState.fan, "AUTO") == 0 ) result = fanAUTO;
  return result << 12;
}

//decode temperature setting
unsigned int getTEMP (uint32_t cmd) {
  byte tempDecode = (cmd & 0xF0) >> 4;
  unsigned int got_temperature = 0;
  for (int i=0; i < 14; i++)
    if (tempDecode == temperature[i]) {
      got_temperature = i + 17;
      break;
    }
  return got_temperature;
}

//create CMD code
uint32_t encodeCMD() { //send constant CMDs or encode from state
  uint32_t result = 0;
  if (delfaState.power == false) result = cmdOFF;
  else {
    sprintf(buf, "%s", delfaState.mode);
    result = 0xB20F00; //set prefix
    // AUTO, COOL, DRY, HEAT, FAN
    if ( strcmp(buf, "AUTO") == 0 ) {
      bitSet(result, 12); //set fan bits to 0001
      bitSet(result, 3); // set mode bits to 1000
      result |= (byte)(temperature[delfaState.temperature - 17] << 4); // encode and set temperature
    } else if ( strcmp(buf, "DRY") == 0 ) {
      bitSet(result, 12); //set fan bits to 0001
      bitSet(result, 2); // set mode bits to 0100
      result |= (byte)(temperature[delfaState.temperature - 17] << 4); // encode and set temperature
    } else if ( strcmp(buf, "FAN") == 0 ) {
      result |= 0xE4; // last byte is always E4 in fan mode
      result |= encodeFAN(); // set fan mode bits
    } else if ( strcmp(buf, "COOL") == 0 ) {
      // mode bits are 0000, no need to set
      result |= 0; //???
      result |= (byte)(temperature[delfaState.temperature - 17] << 4); // encode and set temperature
      result |= encodeFAN(); // set fan mode bits
    } else if ( strcmp(buf, "HEAT" ) == 0 ) {
      result |= 0xC;// set mode bits to 1100
      result |= (byte)(temperature[delfaState.temperature - 17] << 4); // encode and set temperature
      result |= encodeFAN(); // set fan mode bits
    }
  }
  return result;
}

void setup() {
  pinMode(BUILTIN_LED, OUTPUT);     // Initialize the BUILTIN_LED pin as an output
  Serial.begin(115200);
  // init AC state
  delfaState.power = false; // ON or OFF
  sprintf(delfaState.mode, "%s", "COOL");
  delfaState.temperature = 21; //17-30 C
  sprintf(delfaState.fan, "%s", "AUTO");
  delfaState.sleep = false;
  irrecv.enableIRIn();  // Start the receiver
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  irsend.begin(); // Start IR sender
  dht.setup(DHT_PIN, DHTesp::DHT11); //start DHT
  radioSwitch.enableReceive(RADIO_RCV_PIN); //Start 433 receiver
  radioSwitch.enableTransmit(RADIO_SEND_PIN); //Start 433 transmitter
  switchState = EEPROM.read(0); //restore switch state
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
        decodeDelfa((uint32_t)(results.value & 0xFFFFFF));
        irrecv.resume(); // Receive the next value
        delay(200);
      }

      // check 433 receiver
      unsigned long radioCode = 0;
      unsigned long radioNow;
      // TODO Receive timeout
      if (radioSwitch.available()) {
        if ((millis() - radioNow) > 500) { //ignore repeating codes
          radioCode = radioSwitch.getReceivedValue();
          Serial.print("Received ");
          Serial.print(radioCode);
          switch(radioCode) {
            case radioSTREETon:
              Serial.println(" , Street Light On.");
              radioNow = millis();
              bitSet(switchState, 0);
              client.publish("garden/street/streetlight", "ON", true); //Street light on
              break;
            case radioSTREEToff:
              Serial.println(" , Street Light Off.");
              radioNow = millis();
              bitClear(switchState, 0);
              client.publish("garden/street/streetlight", "OFF", true); //Street light off
              break;
            case radioGARDENon:
              Serial.println(" , Garden Light On.");
              radioNow = millis();
              bitSet(switchState, 1);
              client.publish("garden/street/gardenlight", "ON", true); //Garden light on
              break;
            case radioGARDENoff:
              Serial.println(" , Garden Light Off.");
              radioNow = millis();
              bitClear(switchState, 1);
              client.publish("garden/street/gardenlight", "OFF", true); //Garden light off
              break;
            default:
              Serial.println(" , Code unknown.");

          }
          //save switch status
          if (EEPROM.read(0) != switchState) {
            EEPROM.write(0, switchState);
            EEPROM.commit();
          }
        }
        radioSwitch.resetAvailable();
      }
      // blink and report status
      long now = millis();
      int period = 10; // seconds of status renew
      if (now - lastMsg > period * 1000) { // every 10 seconds in production, 2 sec for debug
        // short LED blink
        digitalWrite(BUILTIN_LED, LOW);
        delay(50);
        digitalWrite(BUILTIN_LED, HIGH);
        lastMsg = now;
        value++;
        snprintf (msg, 75, "%ld", value * period);
        Serial.print("Keepalive seconds: ");
        Serial.println(msg);
        client.publish("garden/house/top/ac/status/counter", msg);
        client.publish("garden/house/top/ac/status", "Online");
        client.publish("garden/house/top/ac/status/ip", ip.toString().c_str(), true);

        if ( (value * period) % 30 == 0) pubTopics(); //refresh MQTT every 30s

        //DHT data
        delay(dht.getMinimumSamplingPeriod());
        const char* dhtStatus = dht.getStatusString();
        float humidity = dht.getHumidity();
        float temperature = dht.getTemperature();
        float heatIndex = dht.computeHeatIndex(temperature, humidity, false);
        sprintf(msg,"%i", (int)temperature); client.publish("garden/house/top/climate/temperature", msg);
        sprintf(msg,"%i", (int)humidity); client.publish("garden/house/top/climate/humidity", msg);
        client.publish("garden/house/top/climate/status", dht.getStatusString());

      }
    }
  }
  yield(); //feed WDT
}
