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

//Hardware  
#define SDA_PIN_MAINBOARD    33  /*default 21*/
#define SCL_PIN_MAINBOARD    25  /*default 22*/
#define SDA_PIN_DISPLAY      26
#define SCL_PIN_DISPLAY      27
#define I2C_SLAVE_ADDR        0x27
#define I2C_DISPLAY_ADDR      0x27

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

WebServer server1(80);
WebServer server2(81);
WebServer server3(82);
WebServer *pserver[] = { &server1, &server2, &server3, nullptr };

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
    Task0,   /* Function to implement the task */
    "Task0", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest*/
    &hTask0, /* Task handle. */
    0);      /* Core where the task should run */

  delay(500);

  xTaskCreatePinnedToCore(
    Task1,   /* Function to implement the task */
    "Task1", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest */
    &hTask1, /* Task handle. */
    1);      /* Core where the task should run */
}

// Request:   http://MOWERADRESS/status
// Response:  [rssi dbm];[Cnt_timeout];[Cnt_err];[LstError];
void Web_aktStatus(WebServer *svr)
{
  char out[100] = "";
  long rssi = WiFi.RSSI();

  xSemaphoreTake(SemMutex, 1);  
  sprintf(out, "%ld;%d;%d;%d", rssi, thExchange.Cnt_timeout, thExchange.Cnt_err, thExchange.Lst_err);
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/html", out);
}

// Request:   http://MOWERADRESS/statval
// Response:  [Display];[rssi dbm];[battery];[text]
void Web_aktStatusValues(WebServer *svr)
{
  char out[200] = "";
  long rssi = WiFi.RSSI();
  const char* BatState[] = {"off", "empty", "low", "mid", "full"};
  char point[2] = "";

  xSemaphoreTake(SemMutex, 1);

  if (thExchange.point != ' ')
    sprintf(point, "%c", thExchange.point);
  
  sprintf(out, "%c%c%s%c%c;%ld;%s;%s",
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          point,
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          rssi,
          BatState[thExchange.bat+1],
          DecodeMsg (thExchange.AktDisplay[1], thExchange.AktDisplay[2])
          );
  
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/plain", out);
}

//Webcommand examples: 
// Send command:     http://MOWERADRESS/cmd?parm=start&value=1
// Get Akt Status:   http://MOWERADRESS/cmd?mowerstatus=0
// Get Status Text:  http://MOWERADRESS/cmd?mowerstatus=-E6-
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
  else if (svr->argName(0) == "mowerstatus")
  {
    if (svr->arg(0).length() > 1)
    {
      char buf[svr->arg(0).length() + 1];
      int offset = 0;
    
      if (svr->arg(0).substring(1, 0) == "-")
        offset -= 1;
      
      svr->arg(0).getBytes((unsigned char*)buf, svr->arg(0).length() + 1);
      svr->send(200,"text/plain", String(DecodeMsg (buf[1+offset], buf[2+offset])));
    }
    else
    {
      static char tmpAktDisplay[sizeof thExchange.AktDisplay] = "";
      
      xSemaphoreTake(SemMutex, 1);
      memcpy(tmpAktDisplay, thExchange.AktDisplay, sizeof tmpAktDisplay);
      xSemaphoreGive(SemMutex);

      if (thExchange.cmdQueIdx)
      {
        svr->send(200,"text/plain", "bitte warten...");
        return;
      }
      
      if (!strcmp(tmpAktDisplay, " OFF"))
        svr->send(200,"text/plain", "off");
      else if (!strcmp(tmpAktDisplay, "####"))
        svr->send(200,"text/plain", "---");
      else
        svr->send(200,"text/plain", String(DecodeMsg (tmpAktDisplay[1], tmpAktDisplay[2])));
    }
    return;
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
  int s = 0;

  for (s=0; pserver[s]; s++)
  {
    const char * pngs[] = { 
      "/robomower.png", 
      "/bat_empty.png" ,"/bat_low.png" ,"/bat_mid.png" ,"/bat_full.png",
      "/unlocked.png" ,"/locked.png" ,"/clock.png", 
      nullptr };
    int p = 0;
    
    for (p=0; pngs[p]; p++)
    {
      pserver[s]->on(pngs[p], [=]()
      {
        File dat = SPIFFS.open(pngs[p], "r");
        if (dat) 
        {
          pserver[s]->send(200, "image/png", dat.readString());
          dat.close();
        }
      });
    }
      
    pserver[s]->on("/", [=]()
    {
      File html = SPIFFS.open("/index.html", "r");
      if (html)
      {
        pserver[s]->send(200, "text/html", html.readString());
        html.close();
      }
    });
    
    pserver[s]->on("/cmd",    HTTP_GET, [=]() {Web_getCmd(pserver[s]);});
    pserver[s]->on("/status",           [=]() {Web_aktStatus(pserver[s]);});
    pserver[s]->on("/values",           [=]() {pserver[s]->send(200, "text/plain", thExchange.WebOutDisplay);});
    pserver[s]->on("/statval",          [=]() {Web_aktStatusValues(pserver[s]);});
    pserver[s]->begin();
  }

  while(1)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      for (s=0; pserver[s]; s++)
      {
        pserver[s]->handleClient();
      }
    }
    delay(10);
  }
}

void Task0( void * pvParameters )
{
  TwoWireSlave WireSlave  = TwoWireSlave(0); //ESP32 <-> Motherboard
  TwoWire      WireMaster = TwoWire(1);      //ESP32 <-> Display/Buttons
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
  WireMaster.begin(SDA_PIN_DISPLAY, SCL_PIN_DISPLAY, 100000);

  //crc.setPolynome(0x1021);
  //crc.setStartXOR(0xFFFF);
  //crc.setEndXOR(0xFFFF);

  while(1)
  {
    int err = 0;

    //check WLAN state
    if (WiFi_WasConnected)
    {
      if (millis() - Lst_WiFi_Status > 5000)
      {
        Lst_WiFi_Status = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
          Serial.println(F("WLAN reconnect.."));
          WiFi.disconnect();
          WiFi.reconnect();
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
    while (1)
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
        //crc.add(&DatReadBuff[ReadBuff_Processed], LEN_DISPLAY_RES-2); -> yield() -> error!!
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
      xSemaphoreTake(SemMutex, 1);
      thExchange.Lst_err = err;
      thExchange.Cnt_err++;
      xSemaphoreGive(SemMutex);
      WireSlave.flush();
      IdxReadBuff = ReadBuff_Processed = 0;
      memset(DatReadBuff, 0, sizeof DatReadBuff);
    }

    if (DatMainboard[0])
    {
      Lst_ButtonReqFromMainboard = millis();
      //send request to read buttons
      WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
      WireMaster.write(DatMainboard, LEN_BUTTONS_REQ);
      WireMaster.endTransmission(true);
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
        //crc.add(DisplayRes, LEN_DISPLAY_RES-2); -> yield() -> error!!
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
      if (DatMainboard[1]||DatMainboard[5]) //valid or just forced from timeout
      {
        WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
        WireMaster.write(DatMainboard, LEN_DISPLAY_RES);
        WireMaster.endTransmission(true);
      }

      if (memcmp(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard))
      {
        static unsigned int CntWebOut = 0;
        uint8_t batraw = 0;

        memcpy(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard);
        CntWebOut++;
        
        thExchange.point = ' ';
        if (DatMainboard[5] & 0x02)
          thExchange.point = ':';
        else if (DatMainboard[5] & 0x01)
          thExchange.point = '.';

        batraw = DatMainboard[5] & 0xE0;
        
        thExchange.bat = -1;
        if (DatMainboard[6] & 0x01)
          thExchange.bat = 3;
        else if (batraw == 0xE0)
          thExchange.bat = 2;
        else if (batraw == 0x60)
          thExchange.bat = 1;
        else if (batraw == 0x20)
          thExchange.bat = 0;

        xSemaphoreTake(SemMutex, 1);
        sprintf(thExchange.AktDisplay, "%c%c%c%c",
          DecodeChar (DatMainboard[1]),
          DecodeChar (DatMainboard[2]),
          DecodeChar (DatMainboard[3]),
          DecodeChar (DatMainboard[4]));
          
        if (DecodeChars_IsRun(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|~~|");
        else if (DecodeChars_IsRunReady(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|--|");

        //cnt;seg1;seg2;seg3;seg4;point;lock;clock;bat
        memset(thExchange.WebOutDisplay, 0, sizeof thExchange.WebOutDisplay);
        sprintf(thExchange.WebOutDisplay, "%d;%c;%c;%c;%c;%c;%d;%d;%d",
          CntWebOut,
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          thExchange.point,
          (DatMainboard[5] & 0x08)?1:0,  //lock
          (DatMainboard[5] & 0x04)?1:0,  //clock
          thExchange.bat);
        xSemaphoreGive(SemMutex);
      }
    }
    delay(1);
  }
}

void loop()
{
  //Core 1
}
