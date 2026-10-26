#ifndef UN_SNAPSHOT_H
#define UN_SNAPSHOT_H
#include <stdint.h>
#include <string.h>
/* Lossless compact RCF1 state: car/lap, 10-bit X/Y, heading, speed,
 * waypoint, total time and best lap. Two records fit one UART/UDP batch. */
#define UN_SNAPSHOT_BYTES 11u
static inline uint8_t un_snapshot_enabled(const uint8_t *config){return !memcmp(config,"RCF1SNP2",8);}
static inline void un_snapshot_pack(uint8_t *out,const uint8_t *v){
    uint16_t x=(uint16_t)v[2]|((uint16_t)v[3]<<8),y=(uint16_t)v[4]|((uint16_t)v[5]<<8);
    out[0]=v[0]|(v[9]<<2);out[1]=x;out[2]=(x>>8)|(y<<2);out[3]=y>>6;
    out[4]=v[6];out[5]=v[7];out[6]=v[8];memcpy(out+7,v+10,4);
}
static inline uint8_t un_snapshot_unpack(uint8_t *out,const uint8_t *v){
    if((v[0]&0xe0u) || (v[0]>>2)>4u || v[3]>15u || v[6]>=237u)return 0;
    uint16_t x=v[1]|((uint16_t)(v[2]&3u)<<8),y=(v[2]>>2)|((uint16_t)v[3]<<6);
    out[0]=v[0]&3u;out[1]=0;out[2]=x;out[3]=x>>8;out[4]=y;out[5]=y>>8;
    out[6]=v[4];out[7]=v[5];out[8]=v[6];out[9]=v[0]>>2;memcpy(out+10,v+7,4);
    return 1;
}
#endif
