#include "LX790_util.h"

struct
{
  const char c;
  const char paddern;
} const SegChr[] =
{
  {' ', 0x00},
  {'1', 0x20 | 0x04},
  {'2', 0x01 | 0x04 | 0x08 | 0x10 | 0x40 },
  {'3', 0x01 | 0x04 | 0x08 | 0x20 | 0x40 },
  {'4', 0x02 | 0x08 | 0x04 | 0x20 },
  {'5', 0x01 | 0x02 | 0x08 | 0x20 | 0x40 },
  {'6', 0x01 | 0x02 | 0x08 | 0x10 | 0x20 | 0x40 },
  {'7', 0x01 | 0x04 | 0x20 },
  {'8', 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 },
  {'0', 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | 0x40 },
  {'9', 0x01 | 0x02 | 0x04 | 0x08 | 0x20 | 0x40}, 
  {'E', 0x01 | 0x02 | 0x08 | 0x10 | 0x40 },
  {'r', 0x08 | 0x10 },
  {'o', 0x08 | 0x20 | 0x40 | 0x10},
  {'F', 0x01 | 0x02 | 0x08 | 0x10},
  {'-', 0x08 },
  {'A', 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 },
  {'I', 0x20 | 0x04 }, // special - wie '1'
  {'d', 0x04 | 0x08 | 0x10 | 0x20 | 0x40 },
  {'L', 0x02 | 0x10 | 0x40 },
  {'P', 0x01 | 0x02 | 0x04 | 0x08 | 0x10 },
  {'n', 0x10 | 0x08 | 0x20 },
  {'U', 0x02 | 0x04 | 0x10 | 0x20 | 0x40},
  {'S', 0x01 | 0x02 | 0x08 | 0x20 | 0x40},
  {'b', 0x02 | 0x08 | 0x10 | 0x20 | 0x40},
  {'H', 0x02 | 0x04 | 0x08 | 0x10 | 0x20 },
  {0, 0 }
};

/*****************************************************************************/
struct
{
  const char * Display;
  const char * Str;
} const TblMsg[] =
{
  {"-F1-", "Regenverzögerung aktiviert."},
  {"-E1-", "Der Robi befindet sich außerhalb des Funktionsbereichs."},
  {"-E2-", "Radmotor blockiert."},
  {"-E3-", "Messer blockiert."},
  {"-E4-", "Der Robi steckt fest."},
  {"-E5-", "Der Robi wird hochgehoben."},
  {"-E6-", "Der Robi wird hochgehoben."},
  {"-E7-", "Akkufehler"},
  {"-E8-", "Es dauert zu lange, bis der Robi zur Ladestation zurückkehrt."},
  {"-EE-", "Unbekannter Fehler."},
  {"IdLE", "Warte auf start"},
  {"|ok|", "Mähbereit"},
  {"|~~|", "Mähen..."},
  {"|--|", "Mähen...Hindernis..."},
  {nullptr,"not found"}
};

char DecodeChar (char raw)
{
  int i = 0;
  
  for (i = 0; SegChr[i].c; i++)
  {
    if (SegChr[i].paddern == raw)
    {
      return SegChr[i].c;
    }
  }
  
  return '#';
}

int DecodeChars_IsRun (uint8_t raw[4])
{
  int i = 0;
  int j = 0;
  int cnt = 0;
  
  for (i = 0; i<4; i++)
  {
    for (j = 0; j<8; j++)
    {
      if(raw[i] & 1<<j)
      {
        cnt++;
      }
    }
  }

  return cnt == 1;
}

int DecodeChars_IsRunReady (uint8_t raw[4])
{
  int i = 0;
  const uint8_t readyPad[4] = { 0x01|0x02|0x10|0x40,
                                0x01|0x40,
                                0x01|0x40,
                                0x01|0x04|0x20|0x40 };
  
  for (i = 0; i<4; i++)
  {
    if (raw[i] != readyPad[i])
      return 0;
  }
  return 1;
}

char EncodeSeg (char c)
{
  int i = 0;
  
  for (i = 0; SegChr[i].c; i++)
  {
    if (SegChr[i].c == c)
    {
      return SegChr[i].paddern;
    }
  }
  
  return (0x01 | 0x08 | 0x40);
}

const char * DecodeMsg (char c1, char c2)
{
  int i = 0;
  
  for (i = 0; TblMsg[i].Display; i++)
  {
    if (TblMsg[i].Display[1] == c1 && TblMsg[i].Display[2] == c2 )
    {
      return TblMsg[i].Str;
    }
  }
  
  return "not found";
}

//  Landxcape LX790
//  I2C Reverse engineering
//  
//  ###### Display
//  
//  D  Z1 Z2 Z3 Z4 SY BR CS CS
//  
//  02 08 5B 24 08 E0 01 D0 CE  -> E1 gedimmt
//  02 08 5B 24 08 E0 C9 94 96  -> E1 hell
//  
//  02 08 5B 7B 08 E0 01 05 6F  -> E6 gedimmt
//  02 08 5B 7B 08 E0 C9 41 36  -> E6 hell
//  
//  D:  	Typ
//  
//  Z1-Z4:	Zahl 1-4
//
//        0x01
//         _
//  0x02 |   | 0x04
//         -   0x08
//  0x10 | _ | 0x20
//  
//        0x40
//  
//  SY:	Symbole
//  
//    Schloss    0x08
//    Uhr        0x04
//    Punkte     0x01 und 0x02
//
//    Batterie Gehäuse   0x20
//    Batterie Str. re   0x60
//  
//  BR:    Helligkeit (0x01 bis 0xC9 / 1 - 201)
//
//  CS: 2 Byte Checksumme
//  
//  ###### Taster
//  
//  Master / write -> 01 01 E0 C1
//  Master / read  <- 01 01 78 00 00 00 00 5B EC	<- Taster "ok"
//  Master / read  <- 01 04 78 00 00 00 00 5A AF	<- Taster "Home / runter"
//  Master / read  <- 01 02 78 00 00 00 00 BB 22	<- Taster "Start / hoch"
//  Master / read  <- 01 00 78 00 00 00 00 FB A9	<- Taster "Power"             0x78: 01111000
//  Master / read  <- 01 00 FC 00 00 00 00 2D 02	<- Taster "Stop" (Öffner!)    0xFC: 11111100
//  
//  ###### Unbekannt
//  
//  Master / write -> 04 01 15 3E
//    -> vermutlich eine Option wie z.B. Ultraschall, welche das Mainboard auf vorhandensein abfragt..?
//  
//  ###### Unbekannt
//  
//  Master / write -> 05 01 01 83 fb
//    -> wird nach dem Einschalten ans Display geschickt.. dann nie mehr
//  
//  ###### Checksumme
//
//  https://crccalc.com/
//  CRC-16/GENIBUS
//
//  ###### Notizen
//  
//  Adresse
//  27	00100111
//  Adresse + R/W
//  4E	 01001110 write
//  4F	 01001111
//  
//  Display Beispiele
//  
//  08      0000 1000	-
//  5b	0101 1011	E
//  24  0010 0100	1
//  08	0000 1000	-
//  3F	0011 1111	A
//  7B	0111 1011	6
//  5b	0101 1011	E
//  5D	0101 1101	2