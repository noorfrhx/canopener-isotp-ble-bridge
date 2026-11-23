/* Host-side test of the DQ3xx hack (bridge setting 9) in main/isotp.c.
   Run: cc -Imain -o /tmp/test_dq3xx test/test_dq3xx.c main/isotp.c && /tmp/test_dq3xx */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "isotp.h"
static uint64_t now_us = 1000;
static int sent = 0; static uint8_t last[8]; static uint32_t last_id;
void isotp_user_debug(const char* m, ...) {(void)m;}
int isotp_user_send_can(const uint32_t id, const uint8_t* d, const uint16_t n){ sent++; last_id=id; memcpy(last,d,n<8?n:8); return 0; }
uint64_t isotp_user_get_us(void){ return now_us; }
static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){printf("FAIL: %s\n",msg);fails++;} else printf("ok:   %s\n",msg);}while(0)
int main(void){
  static uint8_t sb[4096], rb[4096], out[4096]; uint16_t osz;
  IsoTpLink l;
  uint8_t pend[8]={0x03,0x7F,0x31,0x78,0xAA,0xAA,0xAA,0xAA};
  uint8_t pos[8]={0x04,0x71,0x01,0xFF,0x00,0xAA,0xAA,0xAA};
  uint8_t neg22[8]={0x03,0x7F,0x22,0x31,0xAA,0xAA,0xAA,0xAA};
  /* 1: hack off -> no FC */
  isotp_init_link(&l,0x7E1,0x7E9,sb,4096,rb,4096); l.stmin_override=900;
  isotp_on_can_message(&l,pend,8); now_us+=5000; isotp_poll(&l);
  CHECK(sent==0,"hack off: 7F 31 78 gets no Continue");
  CHECK(isotp_receive(&l,out,4096,&osz)==ISOTP_RET_OK && osz==3 && out[0]==0x7F,"hack off: reply still delivered to host");
  /* 2: hack on -> FC after stmin */
  isotp_init_link(&l,0x7E1,0x7E9,sb,4096,rb,4096); l.stmin_override=900; l.dq3xx_hack=1; sent=0;
  isotp_on_can_message(&l,pend,8);
  CHECK(l.send_status==ISOTP_SEND_STATUS_BOGUS_CONTINUE,"hack on: Continue owed after 7F 31 78");
  CHECK(isotp_receive(&l,out,4096,&osz)==ISOTP_RET_OK && osz==3 && out[2]==0x78,"hack on: reply delivered to host");
  now_us+=500; isotp_poll(&l); CHECK(sent==0,"hack on: nothing before STmin (900 us)");
  now_us+=500; isotp_poll(&l);
  CHECK(sent==1 && last_id==0x7E1 && last[0]==0x30 && last[1]==0 && last[2]==0,"hack on: 30 00 00 sent on 0x7E1 after STmin");
  CHECK(l.send_status==ISOTP_SEND_STATUS_IDLE,"hack on: back to idle");
  now_us+=5000; isotp_poll(&l); CHECK(sent==1,"hack on: only one Continue");
  /* 3: other replies -> none */
  sent=0; isotp_on_can_message(&l,pos,8); isotp_receive(&l,out,4096,&osz); now_us+=5000; isotp_poll(&l);
  CHECK(sent==0,"hack on: positive 71 reply gets no Continue");
  isotp_on_can_message(&l,neg22,8); isotp_receive(&l,out,4096,&osz); now_us+=5000; isotp_poll(&l);
  CHECK(sent==0,"hack on: 7F 22 (other service) gets no Continue");
  /* 4: new send after owed continue works */
  isotp_on_can_message(&l,pend,8); isotp_receive(&l,out,4096,&osz);
  uint8_t req[2]={0x3E,0x00}; sent=0;
  CHECK(isotp_send(&l,req,2)==ISOTP_RET_OK && sent==1 && last[1]==0x3E,"new host request while Continue owed is still sent");
  printf(fails?"\n%d FAILED\n":"\nALL PASS\n",fails); return fails;
}
