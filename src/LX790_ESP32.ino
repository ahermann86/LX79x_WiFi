#include <Arduino.h>
#include <string.h>
#include <Wire.h>
//#include "CRC16.h"
#include "CRC.h"
#include "ahWireSlave.h"
#include "LX790_util.h"
#include <WiFi.h>
#include <WebServer.h>
#include "SPIFFS.h"
#include <Update.h>

#define DEBUG_SERIAL_PRINT    0

//Hardware  
#define SDA_PIN_MAINBOARD    33  /*default 21*/
#define SCL_PIN_MAINBOARD    25  /*default 22*/
#define SDA_PIN_DISPLAY      26
#define SCL_PIN_DISPLAY      27
#define I2C_SLAVE_ADDR        0x27
#define I2C_DISPLAY_ADDR      0x27
#define OUT_IO               13

//I2C commands
#define TYPE_BUTTONS          0x01
#define LEN_BUTTONS_RES       9
#define TYPE_DISPLAY          0x02
#define LEN_BUTTONS_REQ       4
#define LEN_DISPLAY_RES       9
#define TYPE_UNKNOWN          0x04
#define LEN_UNKNOWN_REQ       4
#define TYPE_UNKNOWN_INIT     0x05
#define LEN_UNKNOWN_INIT_REQ  5
#define LEN_MAINBOARD_MAX     9
#define LEN_CMDQUE           10

//Buttons
#define BTN_BYTE1_OK          0x01
#define BTN_BYTE1_START       0x02
#define BTN_BYTE1_HOME        0x04
#define BTN_BYTE2_STOP        0xFC

const char* ssid     = "DEINESSID";
const char* password = "DEINPASSWORT";
const char* hostname = "LX790 lawn mower";

WebServer server(80);

TaskHandle_t hTask0;   //Hardware: I2C, WiFi...
TaskHandle_t hTask1;   //Web...
SemaphoreHandle_t SemMutex;

const char* Buttons[] = {"io", "start", "home", "ok", "stop", nullptr};

static struct
{
  char    WebOutDisplay[100];
  char    AktDisplay[4+1];
  unsigned long WebInButtonTime[5 /* io, start, home, ok, stop */];
  int     WebInButtonState[5 /* io, start, home, ok, stop */];
  int     Lst_err;
  int     Cnt_err;
  int     Cnt_timeout;
  char    point;
  int     bat;
  int     batCharge;
  struct
  {
    unsigned long T_start;
    uint8_t WebInButton[2 /*byte 1 + byte 2*/] = {0};
    unsigned long T_end;
  }cmdQue[LEN_CMDQUE];
  int cmdQueIdx;
  int LstProcesedcmdQueIdx;
} thExchange;

void Task0( void * pvParameters );
void Task1( void * pvParameters );

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  Serial.print  (F("Build date: "));
  Serial.print  (__DATE__);
  Serial.print  (F(" time: "));
  Serial.println(__TIME__);

  SemMutex = xSemaphoreCreateMutex();
  if (SemMutex == NULL)
    Serial.println(F("init semaphore error"));

  if(!SPIFFS.begin(true))
    Serial.println(F("init SPIFFS error"));

  memset(&thExchange, 0, sizeof thExchange);

  xTaskCreatePinnedToCore(
    Task0,   /* Function to implement the task -> I2C, WiFi*/
    "Task0", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest*/
    &hTask0, /* Task handle. */
    0);      /* Core where the task should run */

  delay(500);

  xTaskCreatePinnedToCore(
    Task1,   /* Function to implement the task -> Webserver*/
    "Task1", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest */
    &hTask1, /* Task handle. */
    1);      /* Core where the task should run */
}

char * GetStatustext (void)
{
  static char statustxt[100] = "";
  
  memset (statustxt, 0, sizeof statustxt);
  
  if (thExchange.cmdQueIdx)
  {
    sprintf(statustxt, "bitte warten...");
  }
  else
  {
    if (thExchange.batCharge)
    {
      strcpy(statustxt, "Laden...");  
    }
    else
    {
      strcpy(statustxt, DecodeMsg (thExchange.AktDisplay));  
    }
  }
  
  return statustxt; 
}

// Request:   http://MOWERADRESS/web
// Response:  [cnt];[Display];[point];[lock];[clock];[bat];[rssi dbm];[Cnt_timeout];[Cnt_err];[LstError];[MowerStatustext]
void Web_aktStatusWeb(WebServer *svr)
{
  char out[400] = "";
  long rssi = WiFi.RSSI();
 
  xSemaphoreTake(SemMutex, 1);
  
  sprintf(out, "%s;%ld;%d;%d;%d;%s", 
    thExchange.WebOutDisplay, 
    rssi, 
    thExchange.Cnt_timeout, 
    thExchange.Cnt_err, 
    thExchange.Lst_err,
    GetStatustext());
    
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/html", out);
}

// Request:   http://MOWERADRESS/statval
// Response:  [DisplayWithDelimiter];[rssi dbm];[batAsText];[MowerStatustext]
void Web_aktStatusValues(WebServer *svr)
{
  char out[200] = "";
  long rssi = WiFi.RSSI();
  const char* BatState[] = {"off", "empty", "low", "mid", "full", "charging"};
  int IdxBatState = 0;
  char point[2] = "";
  
  xSemaphoreTake(SemMutex, 1);
  if (thExchange.batCharge)
    IdxBatState = 5; /*charging*/
  else
    IdxBatState = thExchange.bat+1;
  
  if (thExchange.point != ' ')
    sprintf(point, "%c", thExchange.point);

  sprintf(out, "%c%c%s%c%c;%ld;%s;%s",
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          point,
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          rssi,
          BatState[IdxBatState],
          GetStatustext()
          );
  
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/plain", out);
}

//Webcommand examples: 
// Send command:         http://MOWERADRESS/cmd?parm=[command/button]&value=[state/time]
// Send command example: http://MOWERADRESS/cmd?parm=start&value=1
void Web_getCmd(WebServer *svr)
{
  if (svr->argName(0) == "parm" &&
      svr->argName(1) == "value")
  {
    int i = 0;
    int val = svr->arg(1).toInt();
    
    xSemaphoreTake(SemMutex, 1);

    if (thExchange.cmdQueIdx)
    {
      svr->send(500, "text/plain", "busy...");
      return;
    }

    if (svr->arg(0) == "workzone" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+3500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "timedate" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+3500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_START;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "startmow" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_START;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "homemow" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_HOME;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else
    {
      for (i=0; Buttons[i]; i++)
      {
        if (svr->arg(0) == Buttons[i])
        {
          if (i==0) //OnOff pushbutton
          {
            digitalWrite(OUT_IO, val?LOW:HIGH);
          }
          thExchange.WebInButtonState[i] = val > 0;
          
          if (thExchange.WebInButtonState[i])
          {
            thExchange.WebInButtonTime[i] = val + millis();
          }        
          break;
        }
      }
    }
    thExchange.Cnt_timeout = 0;      
    xSemaphoreGive(SemMutex);
  }
  else
  {
    svr->send(500, "text/plain", "invalid parameter(s)");
    return;
  }

  svr->send(200,"text/plain", "ok");
}

void Task1( void * pvParameters )
{
  const char * pngs[] = { 
    "/robomower.png", 
    "/bat_empty.png" ,"/bat_low.png" ,"/bat_mid.png" ,"/bat_full.png",
    "/unlocked.png" ,"/locked.png" ,"/clock.png", 
    nullptr };
  int p = 0;
  
  for (p=0; pngs[p]; p++)
  {
    server.on(pngs[p], [=]()
    {
      File dat = SPIFFS.open(pngs[p], "r");
      if (dat) 
      {
        server.send(200, "image/png", dat.readString());
        dat.close();
      }
    });
  }
    
  server.on("/", [=]()
  {
    File html = SPIFFS.open("/index.html", "r");
    if (html)
    {
      server.send(200, "text/html", html.readString());
      html.close();
    }
  });
  server.on("/update", HTTP_GET, [=]()
  {
    File html = SPIFFS.open("/update.html", "r");
    if (html)
    {
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", html.readString());
      html.close();
    }
  });
  server.on("/execupdate", HTTP_POST, [=]() 
  {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, [=]() 
  {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) 
    {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) 
      { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) 
    {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) 
      {
        Update.printError(Serial);
      }
    } 
    else if (upload.status == UPLOAD_FILE_END) 
    {
      if (Update.end(true)) 
      { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } 
      else 
      {
        Update.printError(Serial);
      }
    }
  });    
  
  server.on("/cmd",    HTTP_GET, [=]() {Web_getCmd(&server);});
  server.on("/web",              [=]() {Web_aktStatusWeb(&server);});
  server.on("/statval",          [=]() {Web_aktStatusValues(&server);});

  server.begin();
  //server.sendHeader("charset", "utf-8");

  while(1)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      server.handleClient();
    }
    delay(10);
  }
}

void Task0( void * pvParameters )
{
  TwoWireSlave WireSlave  = TwoWireSlave(0); //ESP32 <-> Motherboard
  TwoWire      WireMaster = TwoWire(1);      //ESP32 <-> Display/Buttons
  int ProcInit = 1;
  int WiFi_WasConnected = 0;
  unsigned long Lst_WiFi_Status = 0;
  //CRC16 crc;
  int i = 0;
  int ret = 0;
  unsigned long Lst_ButtonReqFromMainboard = 0;
  uint8_t DatReadBuff[LEN_MAINBOARD_MAX*5] = {0};
  int IdxReadBuff = 0;
  int ReadBuff_Processed = 0;
  uint8_t DatMainboard[LEN_MAINBOARD_MAX] = {0};
  uint8_t Lst_DatMainboard[LEN_MAINBOARD_MAX] = {0};
  uint8_t DisplayRes[LEN_DISPLAY_RES] = {0x01, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0xFB, 0xA9};

  memset(DatMainboard, 0, sizeof DatMainboard);
  pinMode(OUT_IO, OUTPUT);
  digitalWrite(OUT_IO, LOW);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  Lst_WiFi_Status = millis();

  //I2C Slave - Motherboard
  ret = WireSlave.begin(SDA_PIN_MAINBOARD, SCL_PIN_MAINBOARD, I2C_SLAVE_ADDR);
  if (!ret)
  {
    Serial.println(F("I2C slave init failed"));
    while(1);
  }
  WireMaster.begin(SDA_PIN_DISPLAY, SCL_PIN_DISPLAY, 100000UL);

  //crc.setPolynome(0x1021);
  //crc.setStartXOR(0xFFFF);
  //crc.setEndXOR(0xFFFF);
  
  while(1)
  {
    int err = 0;
    
    //after power up
    if (ProcInit && ( (millis() > 10*2000)             /*Timeout*/ ||
                       DatMainboard[LEN_DISPLAY_RES-1] /*Komm. ok*/ ) ) 
    {
      ProcInit = 0;
      digitalWrite(OUT_IO, HIGH);
    }

    //check WLAN state
    if (WiFi_WasConnected)
    {
      if (millis() - Lst_WiFi_Status > 10000)
      {
        Lst_WiFi_Status = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
          Serial.println(F("WLAN reconnect.."));
          WiFi.disconnect();
          WiFi.begin(ssid, password);
        }
      }
    }
    else
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        WiFi_WasConnected = 1;
        Serial.print  (F("WiFi successfully connected with IP: "));
        Serial.println(WiFi.localIP());
      }
      else if (millis() - Lst_WiFi_Status > 1000)
      {
        Lst_WiFi_Status = millis();
        Serial.println(F("WiFi connect.."));
      }
    }
    
    //get state/response from buttons
    while (!ProcInit)
    {
      if (WireMaster.requestFrom(I2C_DISPLAY_ADDR, LEN_DISPLAY_RES) != LEN_DISPLAY_RES)
        break;
      ret = WireMaster.available();
      if (!ret)
        break;

      if (ret != LEN_DISPLAY_RES)
      {
        WireMaster.flush();
        Serial.print  (F("should never see me - L: "));
        Serial.println(__LINE__);
        break;
      }

      i = 0;
      while (WireMaster.available())
        DisplayRes[i++] = WireMaster.read();

      break;
    }

    //read Mainboard data
    while (1)
    {
      memset(DatMainboard, 0, sizeof DatMainboard);

      ret = WireSlave.read_buff(&DatReadBuff[IdxReadBuff], (sizeof DatReadBuff)-IdxReadBuff);

      if (ret < 0)
      {
        err = __LINE__;
        break; //err driver
      }

      IdxReadBuff += ret;

      if ((DatReadBuff[0] == TYPE_BUTTONS) && (IdxReadBuff >= LEN_BUTTONS_REQ))
      {
        static uint8_t Req[] = {0x01, 0x01, 0xE0, 0xC1};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_BUTTONS_REQ);
          ReadBuff_Processed = LEN_BUTTONS_REQ;
        }
      }
      else if ((DatReadBuff[0] == TYPE_UNKNOWN) && (IdxReadBuff >= LEN_UNKNOWN_REQ))
      {
        static uint8_t Req[] = {0x04, 0x01, 0x15, 0x3E};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_UNKNOWN_REQ);
          ReadBuff_Processed = LEN_UNKNOWN_REQ;
        }
      }
      else if ((DatReadBuff[0] == TYPE_UNKNOWN_INIT) && (IdxReadBuff >= LEN_UNKNOWN_INIT_REQ))
      {
        static uint8_t Req[] = {0x05, 0x01, 0x01, 0x83, 0xfb};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_UNKNOWN_INIT_REQ);
          ReadBuff_Processed = LEN_UNKNOWN_INIT_REQ;
        }
      }
      else if (DatReadBuff[0] == TYPE_DISPLAY && IdxReadBuff >= LEN_DISPLAY_RES)
      {
        uint16_t calc_crc = 0xFFFF;
        uint16_t msg_crc = 0x0000;

        //crc.restart();
        //crc.add(&DatReadBuff[ReadBuff_Processed], LEN_DISPLAY_RES-2); -> yield() -> error !! -> ESP32 bug ??
        //calc_crc = crc.getCRC();
        calc_crc = crc16(DatReadBuff, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);

        msg_crc |= (DatReadBuff[LEN_DISPLAY_RES-2]);
        msg_crc |= (DatReadBuff[LEN_DISPLAY_RES-1])<<8;
        if (calc_crc != msg_crc)
        {
          err = __LINE__;
          break; //invalid crc
        }
        else
        {
          if (WiFi.status() == WL_CONNECTED)
          {
            DatReadBuff[5] |= 0x10;  //WiFi Symbol
          
            calc_crc = crc16(DatReadBuff, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);
            DatReadBuff[LEN_DISPLAY_RES-2] = calc_crc & 0xff;
            DatReadBuff[LEN_DISPLAY_RES-1] = calc_crc>>8;
          }

          memcpy(DatMainboard, DatReadBuff, LEN_DISPLAY_RES);
          ReadBuff_Processed = LEN_DISPLAY_RES;
        }
      }

      if (ReadBuff_Processed)
      {
        if (ret == 0)
          delay(5);
      
        memcpy(DatReadBuff, &DatReadBuff[ReadBuff_Processed], (sizeof DatReadBuff)-ReadBuff_Processed);
        IdxReadBuff -= ReadBuff_Processed;
      }
      else
      {
        if (IdxReadBuff >= LEN_MAINBOARD_MAX*3)
        {
          err = __LINE__;
          break; //no match
        }
      }
      ReadBuff_Processed = 0;

      break;
    }

    //valid data from mainboard?
    if (err)
    {   
     #if(DEBUG_SERIAL_PRINT  == 1)
      {
        int i = 0;
        char hex[2] = {0};
        char buff[(sizeof DatReadBuff)*2 + 1] = {0};

        Serial.print  ("Err Slave MB ret: ");
        Serial.print  (ret, DEC);
        Serial.print  (" err: ");
        Serial.print  (err, DEC);
        Serial.print  (" Data Read: ");
        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatReadBuff); i++)
        {
          sprintf(hex, "%02x", DatReadBuff[i]);
          strcat(buff, hex);
        }
        Serial.print(buff);

        Serial.print (" Data MB: ");
        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatMainboard); i++)
        {
          sprintf(hex, "%02x", DatMainboard[i]);
          strcat(buff, hex);
        }
        Serial.print(buff);
        Serial.println(" ");
      }
     #endif

      xSemaphoreTake(SemMutex, 1);
      thExchange.Lst_err = err;
      thExchange.Cnt_err++;
      xSemaphoreGive(SemMutex);
      WireSlave.flush();
      IdxReadBuff = ReadBuff_Processed = 0;
      memset(DatReadBuff, 0, sizeof DatReadBuff);
    }
    
    if (ProcInit && !(millis()%100))
    {
      uint8_t InitDisplay[LEN_MAINBOARD_MAX] = {0};
      char num[4] = {0};
      uint16_t calc_crc = 0xFFFF;

      sprintf(num, "%03lu", millis()/100);

      InitDisplay[0] = TYPE_DISPLAY;
      InitDisplay[1] = EncodeSeg('P');
      InitDisplay[2] = EncodeSeg((uint8_t)num[0]);
      InitDisplay[3] = EncodeSeg((uint8_t)num[1]);
      InitDisplay[4] = EncodeSeg((uint8_t)num[2]);
      InitDisplay[5] = (WiFi.status() == WL_CONNECTED)?0x10:0;  //WiFi Symbol
      InitDisplay[6] = 0xC8;

      calc_crc = crc16(InitDisplay, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);
      InitDisplay[LEN_DISPLAY_RES-2] = calc_crc & 0xff;
      InitDisplay[LEN_DISPLAY_RES-1] = calc_crc>>8;

      WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
      WireMaster.write(InitDisplay, LEN_DISPLAY_RES);
      WireMaster.endTransmission(true);
    }

    if (DatMainboard[0])
    {
      size_t size = 0;
      
      Lst_ButtonReqFromMainboard = millis();

      switch (DatMainboard[0])
      {
        //case TYPE_DISPLAY:
        //  size = LEN_DISPLAY_RES;
        //  break;
        case TYPE_BUTTONS:
          size = LEN_BUTTONS_REQ;
          break;
        case LEN_UNKNOWN_REQ:      //04 01 15 3E
          size = 4;
          break;
        case LEN_UNKNOWN_INIT_REQ: //05 01 01 83 fb
          size = 5;
          break;
      }
      
      if (size)
      {
        WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
        WireMaster.write(DatMainboard, size);
        WireMaster.endTransmission(true);
      }
    }
    
    //Timeout or off?
    if (millis() - Lst_ButtonReqFromMainboard > 100)
    {
      xSemaphoreTake(SemMutex, 1);
      thExchange.Cnt_timeout++;
      memset(&thExchange.WebInButtonTime, 0, sizeof thExchange.WebInButtonTime);
      memset(&thExchange.WebInButtonState, 0, sizeof thExchange.WebInButtonState);
      if (thExchange.cmdQueIdx)
      { 
        memset(thExchange.cmdQue, 0, sizeof thExchange.cmdQue);
        thExchange.cmdQueIdx = 0;
      }
      xSemaphoreGive(SemMutex);
      Lst_ButtonReqFromMainboard = millis();

      WireMaster.flush();
     #if(DEBUG_SERIAL_PRINT  == 1)
      {
        int i = 0;
        char hex[2] = {0};
        char buff[(sizeof DatReadBuff)*2 + 1] = {0};
        int NotEmpty = 0;

        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatReadBuff); i++)
        {
          sprintf(hex, "%02x", DatReadBuff[i]);
          strcat(buff, hex);
          if (DatReadBuff[i])
            NotEmpty = 1;
        }
        if (NotEmpty)
        {
          Serial.print  (" To read: ");
          Serial.print  (IdxReadBuff);
          Serial.print  (" proc: ");
          Serial.print  (ReadBuff_Processed);
          Serial.print  (" dat: ");
          Serial.println(buff);
        }
      }
     #endif

      WireSlave.flush();
      IdxReadBuff = ReadBuff_Processed = 0;
      memset(DatReadBuff, 0, sizeof DatReadBuff);
      
      DatMainboard[0] = TYPE_DISPLAY; //force for web counter
    }

    if (DatMainboard[0] == TYPE_BUTTONS)
    {
      //Inject
      int t = 0;
      uint8_t WebInButton[2 /*byte 1 + byte 2*/] = {0};
      static int LstProcesedcmdQueIdx = 0;
      
      memset(WebInButton, 0, sizeof WebInButton);
      
      xSemaphoreTake(SemMutex, 1);
      
      //Buttons/Actions from que
      if (!thExchange.cmdQueIdx)
        LstProcesedcmdQueIdx = 0;
      while (thExchange.cmdQueIdx)
      {
        unsigned long AktTime = millis();

        if (thExchange.cmdQue[LstProcesedcmdQueIdx].T_end < AktTime)
        {
          LstProcesedcmdQueIdx++;
        }
        if ( (thExchange.cmdQueIdx >= LEN_CMDQUE) ||
            (!thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[0] &&
             !thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[1]))
        {
          memset(thExchange.cmdQue, 0, sizeof thExchange.cmdQue);
          thExchange.cmdQueIdx = 0;
          break;
        }
        if (thExchange.cmdQue[LstProcesedcmdQueIdx].T_start < AktTime)
        {
          WebInButton[0] = thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[0];
          WebInButton[1] = thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[1];
        }
        
        break;
      }
      
      //Buttons from web
      for (t=0; Buttons[t]; t++)
      {
        if ( thExchange.WebInButtonState[t] ||
             thExchange.WebInButtonTime[t] > millis() )
        {
          if (t == 0 /*"io"*/)
            WebInButton[0] |= 0;
          if (t == 1 /*"start"*/)
            WebInButton[0] |= BTN_BYTE1_START;
          if (t == 2 /*"home"*/)
            WebInButton[0] |= BTN_BYTE1_HOME;
          if (t == 3 /*"ok"*/)
            WebInButton[0] |= BTN_BYTE1_OK;
          if (t == 4 /*"stop"*/)
            WebInButton[1] |= BTN_BYTE2_STOP;
        }
        else
        {
          thExchange.WebInButtonTime[t] = 0;
        }
      }
      xSemaphoreGive(SemMutex);
            
      if (WebInButton[0] || WebInButton[1])
      {
        uint16_t calc_crc = 0xFFFF;
        
        //01 02 78 00 00 00 00 BB 22
        //|| || -- Button: stop
        //|| -- Buttons: home, start, ok
        //-- Type

        if (WebInButton[0])
          DisplayRes[1] = WebInButton[0];
        if (WebInButton[1])
          DisplayRes[2] = WebInButton[1];

        //crc.restart();
        //crc.add(DisplayRes, LEN_DISPLAY_RES-2); -> yield() -> error !! -> ESP32 bug ??
        //calc_crc = crc.getCRC();
        calc_crc = crc16(DisplayRes, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);

        DisplayRes[LEN_DISPLAY_RES-2] = calc_crc & 0xff;
        DisplayRes[LEN_DISPLAY_RES-1] = calc_crc>>8;
      }

      ret = WireSlave.write_buff(DisplayRes, LEN_DISPLAY_RES);
      if (ret < 0)
      {
        Serial.print  (F("Ret write "));
        Serial.println(ret, DEC);
      }
    }
    else if (DatMainboard[0] == TYPE_DISPLAY)
    {
      static unsigned long Lst_bat_charge = 0;
      
      if (DatMainboard[LEN_DISPLAY_RES-1]) //valid or just forced from timeout
      {
        WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
        WireMaster.write(DatMainboard, LEN_DISPLAY_RES);
        WireMaster.endTransmission(true);
      }

      if ( memcmp(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard) ||
          (Lst_bat_charge && (millis() - Lst_bat_charge > 1000)) )
      {
        static unsigned int CntWebOut = 0;
        static int Lst_bat = -1;
        static unsigned long Lst_low_is_off = 0;
        uint8_t batraw = 0;
        int bat = -1;
        int batWeb = -1;

        memcpy(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard);
        CntWebOut++;
        
        //battery
        batraw = DatMainboard[5] & 0xE0;
        
        if (!(DatMainboard[5] & 0x40))  //"low" - without battery case
          Lst_low_is_off = millis();
        
        if (DatMainboard[6] & 0x01)
          bat = 3;    //"full"
        else if (batraw == 0xE0)
          bat = 2;    //"mid"
        else if (batraw == 0x60)
          bat = 1;    //"low"
        else if (batraw == 0x20)
          bat = 0;    //"empty"
        
        batWeb = bat;
        if ((batraw == 0x60) && (millis() - Lst_low_is_off > 1000))
          batWeb = 1;    //"low"
        else if ((batraw == 0x20) || (batraw == 0x60) /*blink*/)
          batWeb = 0;    //"empty"
        
        //battery charging ?
        if (Lst_bat != bat)
        {
          if (bat > 1 &&
              Lst_bat < bat)
          {
            Lst_bat_charge = millis();
          }

          Lst_bat = bat;
        }
        
        xSemaphoreTake(SemMutex, 1);
        
        if (Lst_bat_charge && (millis() - Lst_bat_charge < 1000))
        {
          thExchange.batCharge = 1;
        }
        else
        {
          thExchange.batCharge = 0;
          Lst_bat_charge = 0;
        }

        thExchange.point = ' ';
        if (DatMainboard[5] & 0x02)
          thExchange.point = ':';
        else if (DatMainboard[5] & 0x01)
          thExchange.point = '.';

        sprintf(thExchange.AktDisplay, "%c%c%c%c",
          DecodeChar (DatMainboard[1]),
          DecodeChar (DatMainboard[2]),
          DecodeChar (DatMainboard[3]),
          DecodeChar (DatMainboard[4]));
          
        if (DecodeChars_IsRun(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|~~|");
        else if (DecodeChars_IsRunReady(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|ok|");
        
        if (thExchange.batCharge && thExchange.AktDisplay[0] == 0)
          strcpy(thExchange.AktDisplay, "Chrg");

        //reformat
        strcpy(thExchange.AktDisplay, LetterOrNumber(thExchange.AktDisplay));
        //cnt;seg1seg2seg3seg4;point;lock;clock;bat
        memset(thExchange.WebOutDisplay, 0, sizeof thExchange.WebOutDisplay);
        sprintf(thExchange.WebOutDisplay, "%d;%c%c%c%c;%c;%d;%d;%d",
          CntWebOut,
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          thExchange.point,
          (DatMainboard[5] & 0x08)?1:0,  //lock
          (DatMainboard[5] & 0x04)?1:0,  //clock
          bat);
          
        thExchange.bat = batWeb;

        xSemaphoreGive(SemMutex);
        //Serial.println(thExchange.WebOutDisplay);
      }
    }
    delay(1);
  }
}

void loop()
{
  //Core 1
}
