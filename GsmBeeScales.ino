#include <SoftwareSerial.h>
#include <HX711.h>
#include <DallasTemperature.h>

// GSM
/* Comment this out to disable prints and save space */
//#define TINY_GSM_DEBUG Serial
#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

// APN data
#define GPRS_APN       "internet"    // Replace your GPRS APN
//#define GPRS_USER      ""           // Replace with your GPRS user
//#define GPRS_PASSWORD  ""           // Replace with your GPRS password

// Uncomment this to use HTTPS
//#define USE_HTTPS

#define GSM_DTR_PIN 4
SoftwareSerial SerialAT(2, 3); // RX, TX
TinyGsm modem(SerialAT);

void modemSleep();
void modemWakeup();
String getBattStats();
int getSignalQuality();
String sendUSSD(const String &code);
bool gprsConnect();
bool gprsDisconnect();
bool httpRequest(const String& method, const String& url, const String& request, String* response = 0);

// Blynk
const char auth[] = "me7vVcDqmU1X1Rg0qxUrSo3p4qTRPVne";
const char host[] = "fra1.blynk.cloud";

// Timer
#include <arduino-timer.h>
Timer<3> _timer;
Timer<3>::Task _smsTask = 0;

// load cell
HX711 loadCell;
float getWeight();

// temperature
OneWire dsWire(10);
DallasTemperature temperatureSensor(&dsWire);
int getTemperature();

// other
//#define DEBUG_LOGS
String _phoneNumber;
bool _ntpSynced = false;
uint16_t _firstUpdate = 7;
uint16_t _secondUpdate = 19;
const unsigned long _msInHour = 3600000L;
const unsigned long _msInMin = 60000L;

bool sendSmsUpdate(void * = NULL);
bool syncData(void * = NULL);
void updateDataStreams();
bool requestPhoneNumber();
bool requestSmsTimeRange();
unsigned long getSmsTimeout();

void setup()
{
  // Debug console
  Serial.begin(9600);
  delay(10);

  // Set GSM module baud rate
  SerialAT.begin(9600);
  delay(10);

#ifdef GSM_DTR_PIN
  pinMode(GSM_DTR_PIN, OUTPUT);
#endif

  Serial.println("Initializing load cells...");
  loadCell.begin(11, 12); // DT, SCK
  loadCell.set_offset(-205478);
  loadCell.set_scale(-23239);
  loadCell.power_down();
  
  // init temperature sensor
  temperatureSensor.begin();

  modemWakeup();
  Serial.println("Initializing modem...");
  if (!modem.init())
    Serial.println("Init failed");

  Serial.println("Connecting to network...");
  if (modem.waitForNetwork())
    Serial.println("Network: " + modem.getOperator());
  else
    Serial.println("Register in network failed");

  bool start = true;
  syncData(&start);
  sendSmsUpdate();

  _timer.every(_msInHour, syncData);
}

void loop()
{
  _timer.tick<void>();
}

bool sendSmsUpdate(void*)
{
  if (_phoneNumber.length() == 0)
  {
    Serial.println("Skip SMS send. Phone number empty.");
    return false;
  }
  
  modemWakeup();
  
  String text = "Vaha: " + String(getWeight(), 2) + " kg\n";
  text += "Temp: " + String(getTemperature()) + " *C\n";
  text += "Batt: " + getBattStats() + '\n';
  text += "Syhnal: " + String(getSignalQuality()) + "%\n";
  {
    String balance = sendUSSD("*111#");
    text += balance.substring(0, balance.indexOf('\n')) + '\n';
  }
  
  unsigned long timeout = getSmsTimeout();
  text += "Nasptupne SMS cherez:";
  if ((timeout / _msInHour) != 0)
    text += ' ' + String(timeout / _msInHour) + 'h';
  if (((timeout / _msInMin) % 60) != 0)
    text += ' ' + String((timeout / _msInMin) % 60) + 'm';

  Serial.print("Send SMS to ");
  Serial.print(_phoneNumber);
  Serial.println(" with text:");
  Serial.println(text);

  modem.sendSMS(_phoneNumber, text);

  _smsTask = _timer.in(timeout, sendSmsUpdate);
  
  modemSleep();
  return true;
}

bool syncData(void* start)
{
  modemWakeup();

  if (gprsConnect())
  {
    if (!_ntpSynced)
    {
      Serial.println("NTP server sync...");
      byte res = modem.NTPServerSync();
      _ntpSynced = res == 1;
      Serial.print("NTP server sync - ");
      Serial.println(modem.ShowNTPError(res));
      Serial.println(modem.getGSMDateTime(DATE_FULL));
      modem.streamClear();
    }

    if (_ntpSynced)
    {
      if (requestSmsTimeRange() && _phoneNumber.length() != 0 && !start)
      {
        // update SMS timer without sending SMS
        _timer.cancel(_smsTask);
        _smsTask = _timer.in(getSmsTimeout(), sendSmsUpdate);
      }
    }

    if (requestPhoneNumber() && !start)
    {
      _timer.cancel(_smsTask);
      _smsTask = _timer.in(1000, sendSmsUpdate);
    }
    
    updateDataStreams();

    // send ONLINE event
    httpRequest("GET", String("/external/api/logEvent?token=") + auth + "&code=ONLINE", "");
  }
  gprsDisconnect();
  
  modemSleep();
  return true;
}

void updateDataStreams()
{
  float weight = getWeight();
  float voltage = modem.getBattVoltage() / 1000.0;
  int temp = getTemperature();
  int signal = getSignalQuality();

  httpRequest("GET"
    , String("/external/api/batch/update?token=") + auth 
    + "&v0=" + getWeight()
    + "&v1=" + modem.getBattVoltage() / 1000.0
    + "&v2=" + getTemperature()
    + "&v3=" + getSignalQuality()
    , "");
}

bool requestPhoneNumber()
{
  String resp;
  if (httpRequest("GET", String("/external/api/device/meta?token=") + auth + "&metaFieldId=9", "", &resp))
  {
    if (resp.length() != 0)
    {
      int endPos = resp.lastIndexOf('"');
      if (endPos != -1)
      {
        int startPos = resp.lastIndexOf('"', endPos - 1);
        if (startPos != -1)
        {
          String phone = resp.substring(startPos + 1, endPos);
          Serial.print("Phone Number from server: ");
          Serial.println(phone);

          if (_phoneNumber != phone)
          {
            _phoneNumber = phone;
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool requestSmsTimeRange()
{
  String resp;
  if (httpRequest("GET", String("/external/api/device/meta?token=") + auth + "&metaFieldId=10", "", &resp))
  {
    Serial.println(resp);
    if (resp.length() != 0)
    {
      uint16_t from = _firstUpdate;
      uint16_t to = _secondUpdate;
      int startPos = resp.lastIndexOf(':');
      if (startPos != -1)
      {
        // from and to range are in minutes
        to = resp.substring(startPos + 1).toInt() / 60;
      }

      startPos = resp.lastIndexOf(':', startPos - 1);
      if (startPos != -1)
      {
        from = resp.substring(startPos + 1).toInt() / 60;
      }

      if (_firstUpdate != from || _secondUpdate != to)
      {
        Serial.print("New SMS range: from ");
        Serial.print(from);
        Serial.print(" to ");
        Serial.println(to);

        _firstUpdate = from;
        _secondUpdate = to;

        return true;
      }
    }
  }
  return false;
}

unsigned long getSmsTimeout()
{
  int h = 0, m = 0, s = 0;
  unsigned long timeout = 0;
  
  if (_ntpSynced && modem.getNetworkTime(0, 0, 0, &h, &m, &s, 0))
  {
    h = (h + 2) % 24; // + timezone
    Serial.println("Current time " + String(h) + ":" + String(m) + ":" + String(s));

    if (h >= _firstUpdate && h < _secondUpdate)
      timeout = (_secondUpdate - h) * _msInHour;
    else if (h >= _secondUpdate && h < 24)
      timeout = (24 - h + _firstUpdate) * _msInHour; 
    else // h >= 0 && h < _firstUpdate
      timeout = (_firstUpdate - h) * _msInHour;
  
    timeout -= (m * _msInMin) + (s * 1000L);
  }
  else
  {
    Serial.println("Current time unknown");
    timeout = 12L * _msInHour;
  }
  modem.streamClear();
  return timeout;
}

/**************************************************************
 * AT commands stuff
 **************************************************************/

typedef const __FlashStringHelper* GsmConstStr;

void sendAT(const String& cmd) {
  modem.stream.print("AT");
  modem.stream.println(cmd);
#ifdef DEBUG_LOGS
  Serial.println("cmd: " + cmd);
#endif
}

uint8_t waitResponse(uint32_t timeout, GsmConstStr r1,
                     GsmConstStr r2 = NULL, GsmConstStr r3 = NULL)
{
  String data;
  data.reserve(64);
  int index = 0;
  for (unsigned long start = millis(); millis() - start < timeout; ) {
    while (modem.stream.available() > 0) {
      int c = modem.stream.read();
      if (c < 0) continue;
      data += (char)c;
      if (data.indexOf(r1) >= 0) {
        index = 1;
        goto finish;
      } else if (r2 && data.indexOf(r2) >= 0) {
        index = 2;
        goto finish;
      } else if (r3 && data.indexOf(r3) >= 0) {
        index = 3;
        goto finish;
      }
    }
  }
finish:
#ifdef DEBUG_LOGS
  Serial.print("response: ");
  Serial.println(data);
#endif
  return index;
}


uint8_t waitResponse(GsmConstStr r1,
                     GsmConstStr r2 = NULL, GsmConstStr r3 = NULL)
{
  return waitResponse(1000, r1, r2, r3);
}

uint8_t waitOK_ERROR(uint32_t timeout = 1000) {
  return waitResponse(timeout, F("OK\r\n"), F("ERROR\r\n"));
}

// Start the GSM connection
bool gprsConnect()
{
  Serial.println("Connecting to GPRS...");

  sendAT(F("+SAPBR=3,1,\"Contype\",\"GPRS\""));
  waitOK_ERROR();

  sendAT(F("+SAPBR=3,1,\"APN\",\"" GPRS_APN "\""));
  waitOK_ERROR();

#ifdef GPRS_USER
  sendAT(F("+SAPBR=3,1,\"USER\",\"" GPRS_USER "\""));
  waitOK_ERROR();
#endif
#ifdef GPRS_PASSWORD
  sendAT(F("+SAPBR=3,1,\"PWD\",\"" GPRS_PASSWORD "\""));
  waitOK_ERROR();
#endif

  sendAT(F("+CGDCONT=1,\"IP\",\"" GPRS_APN "\""));
  waitOK_ERROR();

  sendAT(F("+CGACT=1,1"));
  if (waitOK_ERROR(150000L) != 1) { return false; }

  // Open bearer for HTTP app
  sendAT(F("+SAPBR=1,1"));
  if (waitOK_ERROR(85000L) != 1) { return false; }
  
  // Query bearer
  sendAT(F("+SAPBR=2,1"));
  if (waitOK_ERROR(30000L) != 1) { return false; }

  Serial.println("GPRS connected");
  return true;
}

bool gprsDisconnect() 
{
  sendAT(F("+SAPBR=0,1"));
  waitOK_ERROR(65000L);

  sendAT(F("+CGACT=0,1"));
  waitOK_ERROR(65000L);

  Serial.println("GPRS disconnected");
  return true;
}

bool httpRequest(const String& method, const String& url, const String& request, String* response)
{
  Serial.print(F("Request: "));
  Serial.print(host);
  Serial.println(url);

  sendAT(F("+HTTPTERM"));
  waitOK_ERROR();

  sendAT(F("+HTTPINIT"));
  waitOK_ERROR();

  sendAT(F("+HTTPPARA=\"CID\",1"));
  waitOK_ERROR();

#ifdef USE_HTTPS
  sendAT(F("+HTTPSSL=1"));
  waitOK_ERROR();
  String urlCmd(F("+HTTPPARA=\"URL\",\"https://"));
#else
  String urlCmd(F("+HTTPPARA=\"URL\",\""));
#endif
  // send directly to stream in case very long url
#ifdef DEBUG_LOGS
  Serial.print("cmd: ");
  Serial.print(urlCmd);
  Serial.print(host);
  Serial.print(url);
  Serial.println("\"");
#endif

  modem.stream.print("AT");
  modem.stream.print(urlCmd);
  modem.stream.print(host);
  modem.stream.print(url);
  modem.stream.println("\"");
  
  waitOK_ERROR();

  if (request.length()) {
    sendAT(F("+HTTPPARA=\"CONTENT\",\"application/json\""));
    waitOK_ERROR();
    sendAT(String(F("+HTTPDATA=")) + request.length() + "," + 10000);
    waitResponse(F("DOWNLOAD\r\n"));
    modem.stream.print(request);
    waitOK_ERROR();
  }

  if (method == "GET") {
    sendAT(F("+HTTPACTION=0"));
  } else if (method == "POST") {
    sendAT(F("+HTTPACTION=1"));
  } else if (method == "HEAD") {
    sendAT(F("+HTTPACTION=2"));
  } else if (method == "DELETE") {
    sendAT(F("+HTTPACTION=3"));
  }
  waitOK_ERROR();

  if (waitResponse(30000L, F("+HTTPACTION:")) != 1) {
    Serial.println("HTTPACTION Timeout");
    return false;
  }
  modem.stream.readStringUntil(',');
  int code = modem.stream.readStringUntil(',').toInt();
  size_t len = modem.stream.readStringUntil('\n').toInt();

  if (code != 200) {
    Serial.print("Error code:");
    Serial.println(code);
    sendAT(F("+HTTPTERM"));
    waitOK_ERROR();
    return false;
  }
  
  if (len > 0 && response) 
  {
#ifdef DEBUG_LOGS
    Serial.print("Response len: ");
    Serial.println(len);
#endif
    response->reserve(len);

    sendAT(String(F("+HTTPREAD=0,")) + len);
    if (waitResponse(F("+HTTPREAD:")) != 1) {
      Serial.println("HTTPREAD Timeout");
      return false;
    }
    len = modem.stream.readStringUntil('\n').toInt();
#ifdef DEBUG_LOGS
    Serial.print("Response len (2): ");
    Serial.println(len);
#endif

    while (len--) {
      while (!modem.stream.available()) {
        delay(1);
      }
      *response += (char)(modem.stream.read());
    }

#ifdef DEBUG_LOGS
    Serial.print("response: ");
    Serial.println(*response);
#endif
    waitOK_ERROR();
  }

  sendAT(F("+HTTPTERM"));
  waitOK_ERROR();

  return true;
}

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

String sendUSSD(const String &code) {
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

void modemSleep()
{
  Serial.println("set GSM modem to sleep");
#ifdef GSM_DTR_PIN
  modem.sendAT(GF("+CSCLK=1"));
  modem.waitResponse();
  digitalWrite(GSM_DTR_PIN, HIGH);
#else
  modem.sendAT(GF("+CSCLK=2")); // automatic mode (sleep after idle for 5s)
  modem.waitResponse();
#endif
}

void modemWakeup()
{
  Serial.println("wake GSM modem up from sleep");
#ifdef GSM_DTR_PIN
  digitalWrite(GSM_DTR_PIN, LOW);
  delay(100);
  modem.streamClear();
  modem.sendAT(GF("+CSCLK=0"));
  modem.waitResponse();
#else
  if (modem.testAT())
  {
    modem.sendAT(GF("+CSCLK=0"));
    modem.waitResponse();
  }
  else
  {
    Serial.println("testAT failed - hard reset module");
  }
#endif
}

// load cell
float getWeight()
{
  loadCell.power_up();
  delay(10);
  float weight = loadCell.get_units(10);
  loadCell.power_down();
  return weight;
}

// temperature
int getTemperature()
{
  temperatureSensor.requestTemperatures();
  return temperatureSensor.getTempCByIndex(0);
}
