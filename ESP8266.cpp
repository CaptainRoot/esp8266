#include <avr/wdt.h>
#include "Arduino.h"
#include "ESP8266.h"

#define debugSer Serial
#define wifi Serial1
#define SVR_CHAN 1
#define BCN_CHAN 2
#define CLI_CHAN 3
#define BUFFER_SIZE 255
#define BEACON_PORT 34807

// A nice prime number for interval so it reduces the likelihood to collide with other intervals
#define BEACON_INT (6733L)

enum connectMode {
  CONNECT_MODE_NONE = 0,
  CONNECT_MODE_SERVER,
  CONNECT_MODE_CLIENT
};

int  _mode;
long _baudrate;
char _ipaddress[16];
char _broadcast[16];
int  _port;
char _device[48];
char _ssid[48];
char _password[24];
bool _beacon;
long _beaconInterval;
long _previousMillis;
int _replyChan;
DataCallback _dcb;
ConnectCallback _ccb;
char _wb[BUFFER_SIZE];
int _wctr = 0;
bool _connected;
bool _forceReset;
bool _sendingData;
int _connectMode;
int _debugLevel = 0;

ESP8266::ESP8266(int mode, long baudrate, int debugLevel)
{
  _mode = mode;
  _baudrate = baudrate;
  _debugLevel = debugLevel;
  _port = 8000;
  _replyChan = 0;
  memset(_ipaddress, 0, 16);
  memset(_broadcast, 0, 16);
  memset(_device, 0, 48);
  memset(_ssid, 0, 48);
  memset(_password, 0, 24);
  _beacon = false;
  _beaconInterval = BEACON_INT;
  _previousMillis = 0;
  _connected = false;
  _forceReset = false;
  _sendingData = false;
}

int ESP8266::initializeWifi(DataCallback dcb, ConnectCallback ccb)
{  
  if (dcb) {
    _dcb = dcb;
  }
  
  if (ccb) {
    _ccb = ccb; 
  }
  
  wifi.begin(_baudrate);
  wifi.setTimeout(5000); 
  
  clearResults();
  
  // check for presence of wifi module
  wifi.println(F("AT"));
  if(!searchResults("OK", 5000, _debugLevel)) {
    return WIFI_ERR_AT;
  } 
  
  clearResults();
  
  // reset WiFi module
  wifi.println(F("AT+RST"));
  if(!searchResults("ready", 5000, _debugLevel)) {
    return WIFI_ERR_RESET;
  }  
  
  clearResults();

  // set the connectivity mode 1=sta, 2=ap, 3=sta+ap
  wifi.print(F("AT+CWMODE="));
  wifi.println(_mode);
    
  clearResults();
  
  return WIFI_ERR_NONE;
}

int ESP8266::connectWifi(char *ssid, char *password)
{
  
  strcpy(_ssid, ssid);
  strcpy(_password, password);
  
  clearResults();

  // set the access point value and connect
  wifi.print(F("AT+CWJAP=\""));
  wifi.print(ssid);
  wifi.print(F("\",\""));
  wifi.print(password);
  wifi.println(F("\""));
  if(!searchResults("OK", 5000, _debugLevel)) {
    return WIFI_ERR_CONNECT;
  }
  
  // enable multi-connection mode
  if (!setLinkMode(1)) {
    return WIFI_ERR_LINK;
  }
  
  // get the IP assigned by DHCP
  getIP();
  
  // get the broadcast address (assumes class c w/ .255)
  getBroadcast();
  
  return WIFI_ERR_NONE;
}

bool ESP8266::disconnectWifi()
{
  
}

void ESP8266::enableWatchDogTimer()
{
  wdt_enable(WDTO_8S);
  _forceReset = true;
}

bool ESP8266::enableBeacon(char *device)
{
  if (device == NULL) {
    _beacon = true;
    return true;
  }

  // you can only beacon if you're a server
  if (_connectMode != CONNECT_MODE_SERVER)
    return false;
  
  bool ret;
  strcpy(_device, device);
  ret = startUDPChannel(BCN_CHAN, _broadcast, BEACON_PORT);
  _beacon = ret;
  return ret;
}

bool ESP8266::disableBeacon()
{
  _beacon = false;
}

bool ESP8266::send(char *data)
{
  int chan;
  if (_connectMode == CONNECT_MODE_SERVER) {
    chan = _replyChan;
  } else {
    chan = CLI_CHAN;
  }

  return sendData(chan, data);
}

void ESP8266::run()
{
  int v;
  char _data[255];
  unsigned long currentMillis = millis();
  
  if (currentMillis - _previousMillis < _beaconInterval) {
    
    // process wifi messages
    while(wifi.available() > 0) {
      v = wifi.read();
      if (v == 10) {
        _wb[_wctr] = 0;
        _wctr = 0;
        
        processWifiMessage();
      } else if (v == 13) {
        // gndn
      } else {
        _wb[_wctr] = v;
        _wctr++;
      }
    }
 
  
  } else {
    if (_beacon == false) return;
    
    // create message text
    char *line1 = "{\"event\": \"beacon\", \"ip\": \"";
    char *line3 = "\", \"port\": ";
    char *line5 = ", \"device\": \"";
    char *line7 = "\"}\r\n";
    
    // convert port to a string
    char p[6];
    memset(p, 0, 6);
    itoa(_port, p, 10);

    // get lenthg of message text
    memset(_data, 0, 255);
    strcat(_data, line1);
    strcat(_data, _ipaddress);
    strcat(_data, line3);
    strcat(_data, p);
    strcat(_data, line5);
    strcat(_data, _device);
    strcat(_data, line7);
    
    if (sendData(BCN_CHAN, _data))
    {
        _previousMillis = currentMillis;
    }
  }
  
  if (_forceReset == true) 
  {  
    if (currentMillis - _previousMillis > _beaconInterval * 3) 
    {
      // If the last successful broadcast was 2 intervals ago then something has gone wrong and we should probably reset
      if (_debugLevel > 0) {    
        debug("Forcing reset");  
      }
        // force reset
      while(1);
    }
  
    // reset the watch dog timer
    wdt_reset();
  }
}

bool ESP8266::startServer(int port, long timeout)
{
  clearResults();
  
  // cache the port number for the beacon
  _port = port;
  
  wifi.print(F("AT+CIPSERVER="));
  wifi.print(SVR_CHAN);
  wifi.print(F(","));
  wifi.println(_port);
  if(!searchResults("OK", 5000, _debugLevel)){
    return false;
  }
  
  // send AT command
  wifi.print(F("AT+CIPSTO="));
  wifi.println(timeout);
  if(!searchResults("OK", 5000, _debugLevel)) {
    return false;
  }
  
  _connectMode = CONNECT_MODE_SERVER;
  return true;
}

bool ESP8266::startClient(char *ip, int port, long timeout)
{
  clearResults();
  
  wifi.print(F("AT+CIPSTART="));
  wifi.print(CLI_CHAN);
  wifi.print(F(",\"TCP\",\""));
  wifi.print(ip);
  wifi.print("\",");
  wifi.println(port);
  
  if (timeout < 1000L) {
    timeout = 1000L;
  }
  
  if(!searchResults("OK", timeout, _debugLevel)) {
    return false;
  }
  
  _connectMode = CONNECT_MODE_CLIENT;
  return true;
}

char *ESP8266::ip()
{
  return _ipaddress;
}

int ESP8266::scan(char *out, int max)
{
  int timeout = 10000;
  int count = 0;
  int c = 0;
  
  if (_debugLevel > 0) {
    char num[6];
    itoa(max, num, 10);
    debug("maximum length of buffer: ");
    debug(num);
  }
  wifi.println(F("AT+CWLAP"));
  long _startMillis = millis();
  do {
    c = wifi.read();
    if ((c >= 0) && (count < max)) {
      // add data to list
      out[count] = c;
      count++;
    }
  } while(millis() - _startMillis < timeout);

  return count;
}

bool ESP8266::closeConnection(void)
{
  int chan;
  if (_connectMode == CONNECT_MODE_SERVER) {
    chan = _replyChan;
  } else {
    chan = CLI_CHAN;
  }
  
  clearResults();
  
  wifi.print("AT+CIPCLOSE=");
  wifi.println(String(chan));
  if(!searchResults("OK", 5000, _debugLevel)) {
    return false;
  }
  
  return true;
}

// *****************************************************************************
// PRIVATE FUNCTIONS BELOW THIS POINT
// *****************************************************************************

// process a complete message from the WiFi side
// and send the actual data payload to the serial port
void ESP8266::processWifiMessage() {
  int packet_len;
  int channel;
  char *pb;  
  
  // if the message is simply "Link", then we have a live connection
  if(strncmp(_wb, "Link", 5) == 0) {
    
    // flag the connection as active
    _connected = true;
    
    // if a connection callback is set, call it
    if (_ccb) _ccb();
  } else
  
  // message packet received from the server
  if(strncmp(_wb, "+IPD,", 5)==0) {
    
    // get the channel and length of the packet
    sscanf(_wb+5, "%d,%d", &channel, &packet_len);
    
    // cache the channel ID - this is used to reply
    _replyChan = channel;
                
    // if the packet contained data, move the pointer past the header
    if (packet_len > 0) {
      pb = _wb+5;
      while(*pb!=':') pb++;
      pb++;
      
      // execute the callback passing a pointer to the message
      if (_dcb) {
        clearResults();
        _dcb(pb);        
      }
            
      // DANGER WILL ROBINSON - there is no ring buffer or other safety net here.
      // the application should either use the data immediately or make a copy of it!
      // do NOT block in the callback or bad things may happen
      
    }
  } else {
    // other messages might wind up here - some useful, some not.
    // you might look here for things like 'Unlink' or errors
  }
}

bool ESP8266::sendData(int chan, char *data) 
{
  if (_sendingData == true) {
    return false;
  }
  
  _sendingData = true;

  clearResults(500L); 

  // start send
  wifi.print(F("AT+CIPSEND="));
  wifi.print(chan);
  wifi.print(",");
  wifi.println(strlen(data));
  
  // send the data
  wifi.println(data);
  
  int result = searchResults("SEND OK", 10000, _debugLevel);
  
  _sendingData = false;
  
  return result;  
}

bool ESP8266::setLinkMode(int mode) {
  clearResults();
  wifi.print(F("AT+CIPMUX="));
  wifi.println(mode);
  if(!searchResults("OK", 5000, _debugLevel)){
    return false;
  }
  return true;
}

bool ESP8266::startUDPChannel(int chan, char *address, int port) {
  clearResults();
  wifi.print(F("AT+CIPSTART="));
  wifi.print(chan);
  wifi.print(F(",\"UDP\",\""));
  wifi.print(address);
  wifi.print("\",");
  wifi.println(port);
  if(!searchResults("OK", 5000, _debugLevel)) {
    return false;
  }
  return true;
}

// private convenience functions
bool ESP8266::getIP() {
  
  char c;
  char buf[15];
  int dots, ptr = 0;
  bool ret = false;

  memset(buf, 0, 15);

  wifi.println(F("AT+CIFSR"));
  delay(500);
  while (wifi.available() > 0) {
    c = wifi.read();
    
    // increment the dot counter if we have a "."
    if ((int)c == 46) {
      dots++;
    }
    if ((int)c == 10) {
      // end of a line.
      if ((dots == 3) && (ret == false)) {
        buf[ptr] = 0;
        strcpy(_ipaddress, buf);
        ret = true;
      } else {
        memset(buf, 0, 15);
        dots = 0;
        ptr = 0;
      }
    } else
    if ((int)c == 13) {
      // ignore it
    } else {
      buf[ptr] = c;
      ptr++;
    }
  }
  
  if (_debugLevel > 0) {
    debug(F("DBG Get IP:"));
    debug(_ipaddress);
  }

  return ret;
}


bool ESP8266::getBroadcast() {
  
  int i, c, dots = 0;
  
  if (strlen(_ipaddress) < 7) {
    return false;
  }
  
  memset(_broadcast, 0, 16);
  for (i = 0; i < strlen(_ipaddress); i++) {
    c = _ipaddress[i];
    _broadcast[i] = c;
    if (c == 46) dots++;
    if (dots == 3) break;
  }
  i++;
  _broadcast[i++] = 50;
  _broadcast[i++] = 53;
  _broadcast[i++] = 53;
  _broadcast[i] = 0;
  
  if (_debugLevel > 0) {
    debug(F("DBG Get Broadcast:"));
    debug(_broadcast);
  }
  
  return true;
}

void ESP8266::debug(char *msg) {
  if (_debugLevel > 0) {
    debugSer.println(msg);
  } 
}

void ESP8266::debug(const __FlashStringHelper*msg)
{
  if (_debugLevel > 0) {
    debugSer.println(msg);
  }
}

bool ESP8266::searchResults(char *target, long timeout, int dbg)
{
  if (dbg > 1) {
    char targetMsg[100];
    sprintf(targetMsg, "Search Target: %s", target);
    debug(targetMsg);
  }
      
  int c;
  int index = 0;
  int targetLength = strlen(target);
  int count = 0;
  char _data[255];
  
  memset(_data, 0, 255);

  long _startMillis = millis();
  do {
    c = wifi.read();
    if (c >= 0) {
      if (dbg > 0) {
        if (count >= 254) {
          debug(_data);
          memset(_data, 0, 255);
          count = 0;
        }
        _data[count] = c;
        count++;
      }

      if (c != target[index])
        index = 0;
        
      if (c == target[index]){
        if(++index >= targetLength){
          if (dbg > 1)
            debug(_data);
            
          debug("Search Found!");  
                        
          return true;
        }
      }
    }
  } while(millis() - _startMillis < timeout);

  if (dbg > 1) {
    debug("Fail on search results");
    if (_data[0] == 0) {
      debug("Reason: No data");
    } else {
      debug("Instead received start---");
      debug(_data);
      debug("Instead received end---");      
    }
  }

  return false;
}

void ESP8266::clearResults(unsigned long minTimeMillis) {
  char c;
  unsigned long startTime = millis();
  
  // Get everything in the buffer first  
  while (wifi.available() > 0) {
    c = wifi.read();
  }
  
  // ensure that we wait for at least minTimeMillis milliseconds.
  while (millis() - startTime < minTimeMillis) {
    while (wifi.available() > 0) {
      c = wifi.read();
    }
    delay(200);
  }
  
  // Get any left overs.
  while (wifi.available() > 0) {
    c = wifi.read();
  }
}
