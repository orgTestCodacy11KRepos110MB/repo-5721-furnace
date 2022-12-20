/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2022 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "pokey.h"
#include "../engine.h"
#include "../../ta-log.h"

#define rWrite(a,v) if (!skipRegisterWrites) {writes.emplace(a,v); if (dumpWrites) {addWrite(a,v);} }

#define CHIP_DIVIDER 1

const char* regCheatSheetPOKEY[]={
  "AUDF1", "0",
  "AUDC1", "1",
  "AUDF2", "2",
  "AUDC2", "3",
  "AUDF3", "4",
  "AUDC3", "5",
  "AUDF4", "6",
  "AUDC4", "7",
  "AUDCTL", "8",
  NULL
};

const char** DivPlatformPOKEY::getRegisterSheet() {
  return regCheatSheetPOKEY;
}

void DivPlatformPOKEY::acquire(short* bufL, short* bufR, size_t start, size_t len) {
  for (size_t h=start; h<start+len; h++) {
    while (!writes.empty()) {
      QueuedWrite w=writes.front();
      Update_pokey_sound_mz(&pokey,w.addr,w.val,0);
      regPool[w.addr&0x0f]=w.val;
      writes.pop();
    }

    mzpokeysnd_process_16(&pokey,&bufL[h],1);
  }
}

void DivPlatformPOKEY::tick(bool sysTick) {
  for (int i=0; i<4; i++) {
    chan[i].std.next();
    if (chan[i].std.vol.had) {
      chan[i].outVol=VOL_SCALE_LINEAR(chan[i].vol&15,MIN(15,chan[i].std.vol.val),15);
      chan[i].ctlChanged=true;
    }
    if (NEW_ARP_STRAT) {
      chan[i].handleArp();
    } else if (chan[i].std.arp.had) {
      if (!chan[i].inPorta) {
        chan[i].baseFreq=NOTE_PERIODIC(parent->calcArp(chan[i].note,chan[i].std.arp.val));
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.wave.had) {
      chan[i].wave=chan[i].std.wave.val;
      chan[i].ctlChanged=true;
      chan[i].freqChanged=true;
    }
    if (chan[i].std.pitch.had) {
      if (chan[i].std.pitch.mode) {
        chan[i].pitch2+=chan[i].std.pitch.val;
        CLAMP_VAR(chan[i].pitch2,-32768,32767);
      } else {
        chan[i].pitch2=chan[i].std.pitch.val;
      }
      chan[i].freqChanged=true;
    }
  }

  if (audctlChanged) {
    audctlChanged=false;
    rWrite(8,audctl);
    for (int i=0; i<4; i++) {
      chan[i].freqChanged=true;
      chan[i].ctlChanged=true;
    }
  }

  for (int i=0; i<4; i++) {
    if (chan[i].freqChanged || chan[i].keyOn || chan[i].keyOff) {
      chan[i].freq=parent->calcFreq(chan[i].baseFreq,chan[i].pitch,chan[i].fixedArp?chan[i].baseNoteOverride:chan[i].arpOff,chan[i].fixedArp,true,0,chan[i].pitch2,chipClock,CHIP_DIVIDER);

      if ((i==0 && !(audctl&64)) || (i==2 && !(audctl&32)) || i==1 || i==3) {
        chan[i].freq+=chan[i].freq>>3;
        chan[i].freq>>=4;
        if (chan[i].freq&1) chan[i].freq++;
        chan[i].freq>>=1;
      }

      if (audctl&1) {
        chan[i].freq>>=2;
      }

      if ((i==0 && audctl&16) || (i==2 && audctl&8)) {
        if (chan[i].freq>65535) chan[i].freq=65535;
      } else {
        if (chan[i].freq>255) chan[i].freq=255;
      }

      if (--chan[i].freq<0) chan[i].freq=0;

      // write frequency
      if ((i==1 && audctl&16) || (i==3 && audctl&8)) {
        // ignore - channel is paired
      } else {
        rWrite(i<<1,chan[i].freq);
        logV("%d: %d",i,chan[i].freq);
      }

      if (chan[i].keyOff) {
        chan[i].ctlChanged=true;
      }
      if (chan[i].keyOn) chan[i].keyOn=false;
      if (chan[i].keyOff) chan[i].keyOff=false;
      chan[i].freqChanged=false;
    }
    if (chan[i].ctlChanged) {
      unsigned char val=((chan[i].active && !isMuted[i])?(chan[i].outVol&15):0)|(chan[i].wave<<5);
      if ((i==1 && audctl&16) || (i==3 && audctl&8)) {
        // mute - channel is paired
        val=0;
      }
      chan[i].ctlChanged=false;
      rWrite(1+(i<<1),val);
    }
  }
}

int DivPlatformPOKEY::dispatch(DivCommand c) {
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,DIV_INS_POKEY);
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].baseFreq=NOTE_PERIODIC(c.value);
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      chan[c.chan].active=true;
      chan[c.chan].keyOn=true;
      chan[c.chan].macroInit(ins);
      if (!parent->song.brokenOutVol && !chan[c.chan].std.vol.will) {
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      chan[c.chan].insChanged=false;
      break;
    }
    case DIV_CMD_NOTE_OFF:
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].macroInit(NULL);
      break;
    case DIV_CMD_NOTE_OFF_ENV:
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].std.release();
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
        chan[c.chan].insChanged=true;
      }
      break;
    case DIV_CMD_VOLUME:
      if (chan[c.chan].vol!=c.value) {
        chan[c.chan].vol=c.value;
        if (!chan[c.chan].std.vol.has) {
          chan[c.chan].outVol=c.value;
          if (chan[c.chan].active) {
            chan[c.chan].ctlChanged=true;
          }
        }
      }
      break;
    case DIV_CMD_GET_VOLUME:
      if (chan[c.chan].std.vol.has) {
        return chan[c.chan].vol;
      }
      return chan[c.chan].outVol;
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_WAVE:
      chan[c.chan].wave=c.value;
      chan[c.chan].ctlChanged=true;
      break;
    case DIV_CMD_NOTE_PORTA: {
      int destFreq=NOTE_PERIODIC(c.value2);
      bool return2=false;
      if (destFreq>chan[c.chan].baseFreq) {
        chan[c.chan].baseFreq+=c.value;
        if (chan[c.chan].baseFreq>=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      } else {
        chan[c.chan].baseFreq-=c.value;
        if (chan[c.chan].baseFreq<=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      }
      chan[c.chan].freqChanged=true;
      if (return2) {
        chan[c.chan].inPorta=false;
        return 2;
      }
      break;
    }
    case DIV_CMD_LEGATO:
      chan[c.chan].baseFreq=NOTE_PERIODIC(c.value);
      chan[c.chan].freqChanged=true;
      chan[c.chan].note=c.value;
      break;
    case DIV_CMD_PRE_PORTA:
      if (chan[c.chan].active && c.value2) {
        if (parent->song.resetMacroOnPorta) chan[c.chan].macroInit(parent->getIns(chan[c.chan].ins,DIV_INS_POKEY));
      }
      if (!chan[c.chan].inPorta && c.value && !parent->song.brokenPortaArp && chan[c.chan].std.arp.will && !NEW_ARP_STRAT) chan[c.chan].baseFreq=NOTE_PERIODIC(chan[c.chan].note);
      chan[c.chan].inPorta=c.value;
      break;
    case DIV_CMD_GET_VOLMAX:
      return 15;
      break;
    case DIV_CMD_MACRO_OFF:
      chan[c.chan].std.mask(c.value,true);
      break;
    case DIV_CMD_MACRO_ON:
      chan[c.chan].std.mask(c.value,false);
      break;
    case DIV_ALWAYS_SET_VOLUME:
      return 1;
      break;
    default:
      break;
  }
  return 1;
}

void DivPlatformPOKEY::muteChannel(int ch, bool mute) {
  isMuted[ch]=mute;
  chan[ch].ctlChanged=true;
}

void DivPlatformPOKEY::forceIns() {
  for (int i=0; i<4; i++) {
    chan[i].insChanged=true;
    chan[i].ctlChanged=true;
    chan[i].freqChanged=true;
  }
}

void* DivPlatformPOKEY::getChanState(int ch) {
  return &chan[ch];
}

DivMacroInt* DivPlatformPOKEY::getChanMacroInt(int ch) {
  return &chan[ch].std;
}

DivDispatchOscBuffer* DivPlatformPOKEY::getOscBuffer(int ch) {
  return oscBuf[ch];
}

unsigned char* DivPlatformPOKEY::getRegisterPool() {
  return regPool;
}

int DivPlatformPOKEY::getRegisterPoolSize() {
  return 9;
}

void DivPlatformPOKEY::reset() {
  while (!writes.empty()) writes.pop();
  memset(regPool,0,16);
  for (int i=0; i<4; i++) {
    chan[i]=DivPlatformPOKEY::Channel();
    chan[i].std.setEngine(parent);
  }
  if (dumpWrites) {
    addWrite(0xffffffff,0);
  }

  ResetPokeyState(&pokey);

  audctl=0;
  audctlChanged=true;
}

bool DivPlatformPOKEY::keyOffAffectsArp(int ch) {
  return true;
}

void DivPlatformPOKEY::notifyInsDeletion(void* ins) {
  for (int i=0; i<4; i++) {
    chan[i].std.notifyInsDeletion((DivInstrument*)ins);
  }
}

void DivPlatformPOKEY::setFlags(const DivConfig& flags) {
  if (flags.getInt("clockSel",0)) {
    chipClock=COLOR_PAL*2.0/5.0;
  } else {
    chipClock=COLOR_NTSC/2.0;
  }
  CHECK_CUSTOM_CLOCK;
  rate=chipClock;
  for (int i=0; i<4; i++) {
    oscBuf[i]->rate=rate/16;
  }
}

void DivPlatformPOKEY::poke(unsigned int addr, unsigned short val) {
  rWrite(addr,val);
}

void DivPlatformPOKEY::poke(std::vector<DivRegWrite>& wlist) {
  for (DivRegWrite& i: wlist) rWrite(i.addr,i.val);
}

int DivPlatformPOKEY::init(DivEngine* p, int channels, int sugRate, const DivConfig& flags) {
  parent=p;
  dumpWrites=false;
  skipRegisterWrites=false;
  for (int i=0; i<4; i++) {
    isMuted[i]=false;
    oscBuf[i]=new DivDispatchOscBuffer;
  }

  MZPOKEYSND_Init(&pokey);
  
  setFlags(flags);
  reset();
  return 6;
}

void DivPlatformPOKEY::quit() {
  for (int i=0; i<4; i++) {
    delete oscBuf[i];
  }
}

DivPlatformPOKEY::~DivPlatformPOKEY() {
}
