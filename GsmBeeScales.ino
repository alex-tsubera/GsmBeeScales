#include <SoftwareSerial.h>
#include <HX711.h>
#include <DallasTemperature.h>

/* Comment this out to disable prints and save space */
//#define TINY_GSM_DEBUG Serial

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>
#include <BlynkApiArduino.h>
//#define BLYNK_TEMPLATE_ID "TMPLQ3ddsmFe"
//#define BLYNK_DEVICE_NAME "BeeHive"
//#define BLYNK_AUTH_TOKEN "-LxMQAUmpFQhCGiFLIzIw17E93mnoJg3"

#define MS_IN_SEC 1000L
#define MS_IN_MIN (60L * MS_IN_SEC)
#define MS_IN_HOUR (60L * MS_IN_MIN)
#define MS_IN_DAY (24L * MS_IN_HOUR)
#define TIMEZONE 2
#define FIRST_UPDATE 8
#define SECOND_UPDATE 21
//#define DTR_PIN 4
//#define DEFAULT_NUMBER "+380665553534"
//#define DEFAULT_NUMBER "+380977907206"
//#define DEFAULT_NUMBER "+380983338870"

bool ntpSynced = false;

// temperature
OneWire dsWire(6);
DallasTemperature temperatureSensor(&dsWire);

// gsm
SoftwareSerial SerialAT(2, 3); // RX, TX
TinyGsm modem(SerialAT);
BlynkTimer timer;

// load cell
HX711 loadCell;

void initLoadCell()
{
  Serial.println("Initializing load cells...");
  loadCell.begin(11, 12);
  loadCell.set_offset(693092);
  loadCell.set_scale(-24293);
  loadCell.power_down();
}

float getWeight()
{
  loadCell.power_up();
  delay(10);
  float weight = loadCell.get_units(5);
  loadCell.power_down();
  return weight;
}

// GSM
String getBattStats()
{
  uint8_t chargeState = 0;
  int8_t percent = 0;
  uint16_t mVolts = 0;
  modem.getBattStats(chargeState, percent, mVolts);
  char str[16] = {0};
  sprintf(str, "%d mV (%d%%)", mVolts, percent);
  return str;
}

int getSignalQuality()
{
  int value = modem.getSignalQuality();
  if (value == 99)
    return 0;
  return (value / 31.0) * 100;
}

String sendUSSD(String code) {
  // Set preferred message format to text mode
  modem.sendAT(GF("+CMGF=1"));
  modem.waitResponse();
  // Set GSM 7 bit default alphabet (3GPP TS 23.038)
  modem.sendAT(GF("+CSCS=\"GSM\""));
  modem.waitResponse();
  // Send the message
  modem.sendAT(GF("+CUSD=1,\""), code, GF("\""));
  if (modem.waitResponse() != 1) { return ""; }
  if (modem.waitResponse(10000L, GF("+CUSD:")) != 1) { return ""; }
  modem.stream.readStringUntil('"');
  String hex = modem.stream.readStringUntil('"');
  modem.streamClear();
  return hex;
}

// temperature
int getTemperature()
{
  temperatureSensor.requestTemperatures();
  return temperatureSensor.getTempCByIndex(0);
}

unsigned long getUpdateTimeout()
{
  int h = 0, m = 0, s = 0;
  unsigned long timeout = 0;
  
  if (ntpSynced && modem.getNetworkTime(0, 0, 0, &h, &m, &s, 0))
  {
    h = (h + TIMEZONE) % 24;
    Serial.println("Current time " + String(h) + ":" + String(m) + ":" + String(s));
  
    if (h >= FIRST_UPDATE && h < SECOND_UPDATE)
      timeout = (SECOND_UPDATE - h) * MS_IN_HOUR;
    else if (h >= SECOND_UPDATE && h < 24)
      timeout = (24 - h + FIRST_UPDATE) * MS_IN_HOUR; 
    else // h >= 0 && h < FIRST_UPDATE
      timeout = (FIRST_UPDATE - h) * MS_IN_HOUR;
  
    timeout -= m * MS_IN_MIN + s * MS_IN_SEC; 
  }
  else
  {
    Serial.println("Current time unknown");
    timeout = MS_IN_DAY;
  }
  modem.streamClear();
  return timeout;
}

void modemSleep()
{
  //    Serial.println("set GSM modem to sleep");
//  digitalWrite(DTR_PIN, HIGH);
  modem.sendAT(GF("+CSCLK=2")); // automatic mode (sleep after idle for 5s)
  modem.waitResponse();
//  modem.sleepEnable(); // requires DTR pin
}
void modemWakeup()
{
//    Serial.println("wake GSM modem up from sleep");
//  digitalWrite(DTR_PIN, LOW);
//  delay(100);
  if (modem.testAT())
  {
    modem.sendAT(GF("+CSCLK=0"));
    modem.waitResponse();
  }
  else
    Serial.println("testAT failed - hard reset module");
}

void sendSmsUpdate()
{
  modemWakeup();
  modem.streamClear();

  unsigned long timeout = getUpdateTimeout();
  String balance = sendUSSD("*111#");
  
  String text = "Weight: " + String(getWeight(), 2) + " kg\n";
  text += "Temp: " + String(getTemperature()) + " *C\n";
  text += "Battery: " + getBattStats() + "\n";
  text += "Signal: " + String(getSignalQuality()) + "%\n";
  text += balance.substring(0, balance.indexOf('\n')) + "\n";
  text += "Next update in: " + String(timeout / MS_IN_HOUR) + " h ";
  if (((timeout / MS_IN_MIN) % 60) != 0)
    text += String((timeout / MS_IN_MIN) % 60) + " m";
    
  Serial.print("Send SMS to ");
  Serial.print(DEFAULT_NUMBER);
  Serial.println(" with text:");
  Serial.println(text);
  
  modem.sendSMS(DEFAULT_NUMBER, text);
  timer.setTimeout(timeout, sendSmsUpdate);
  modemSleep();
}

void setup()
{
//  pinMode(DTR_PIN, OUTPUT);
  // Debug console
  Serial.begin(9600);
  delay(10);

  initLoadCell();
  temperatureSensor.begin();

  // Set GSM module baud rate
  SerialAT.begin(9600);
  delay(10);

  Serial.println("Initializing modem...");
  modemWakeup();
  if (!modem.init())
    Serial.println("Init failed");

  Serial.println("Connecting to network...");
  if (modem.waitForNetwork())
    Serial.println("Network: " + modem.getOperator());
  else
    Serial.println("Register in network failed");

//  Serial.println("Connecting to internet...");
//  if (modem.gprsConnect("internet", "", ""))
//  {
//    Serial.println("NTP server sync...");
//    byte res = modem.NTPServerSync();
//    ntpSynced = res == 1;
//    Serial.println("NTP server sync - " + modem.ShowNTPError(res));
//    Serial.println(modem.getGSMDateTime(DATE_FULL));
//  }
//  else
//    Serial.println("Connection to internet failed.");
//  modem.gprsDisconnect();
  
  sendSmsUpdate();
}

void loop()
{
  timer.run();
}
