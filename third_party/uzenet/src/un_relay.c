#ifndef UN_RELAY_H
#include "un_relay.h"
#endif
#include <string.h>
uint32_t ur_get32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
void ur_put32(uint8_t *p,uint32_t n) {unsigned i;for(i=0;i<4;i++){p[i]=(uint8_t)n;n>>=8;}}
int ur_seal(uint8_t *p,const uint8_t key[32],uint8_t domain,
            const uint8_t match[8],uint8_t peer,uint8_t type,uint32_t seq,
            uint32_t frame,uint8_t mask,uint8_t width,const uint8_t pads[8]) {
    if(!seq)return 0;
    unsigned bytes=UR_PAYLOAD(type,width),body=24+bytes;
    if(bytes>32 || (width&63)>4 || ((width&63)>2 && (width>>6) && UR_IS_INPUT(type)))return 0;
    memset(p,0,body);memcpy(p,"UNR2",4);p[4]=type;p[5]=peer;
    memcpy(p+6,match,8);ur_put32(p+14,seq);ur_put32(p+18,frame);p[22]=mask;p[23]=width;
    if(pads)memcpy(p+24,pads,bytes);
    return ur_mac(key,domain,p,body,p+body);
}
int ur_replay(UrReplay *r,uint32_t seq) {
    uint32_t delta;
    if(!seq)return 0;
    if(seq>r->high){delta=seq-r->high;r->bits=delta>=64?1:(r->bits<<delta)|1;r->high=seq;return 1;}
    delta=r->high-seq;
    if(delta>=64 || (r->bits&((uint64_t)1<<delta)))return 0;
    r->bits|=(uint64_t)1<<delta;return 1;
}
int ur_open(const uint8_t *p,size_t n,const uint8_t key[32],uint8_t domain,
            const uint8_t match[8],UrReplay *replay) {
    uint8_t tag[16],diff=0;unsigned i;
    if(n<UR_PACKET || n!=UR_SIZE(p) || (p[23]&63)<1 || (p[23]&63)>4 ||
       memcmp(p,"UNR2",4) || memcmp(p+6,match,8))return 0;
    if(UR_IS_OOB(p[4])){
        if(n!=(UR_IS_OOB_DATA(p[4])?UR_PACKET_MAX:UR_PACKET))return 0;
    }else if(n>UR_PACKET && ((p[4]!=UR_INPUT && p[4]!=UR_INPUT_DIRECT && p[4]!=UR_FRAME && p[4]!=UD_INPUT) ||
       ur_get32(p+18)<(p[23]>>6)))return 0;
    if(UR_IS_INPUT(p[4]) && (p[23]&63)>2 && (p[23]>>6))return 0;
    if(!ur_mac(key,domain,p,n-16,tag))return 0;
    for(i=0;i<16;i++)diff|=tag[i]^p[n-16+i];
    return !diff && ur_replay(replay,ur_get32(p+14));
}
int ur_history_init(UrHistory *h,uint8_t all,uint8_t local,uint8_t width) {
    if(!all || all>15 || (local&~all) || (width<1 || width>4))return 0;
    memset(h,0,sizeof(*h));h->all=all;h->local=local;h->width=width;return 1;
}
UrFrame *ur_frame(UrHistory *h,uint32_t frame,int create) {
    UrFrame *f;
    if(frame<h->floor || frame-h->floor>=UR_WINDOW || frame>=0xffffffc0UL)return 0;
    f=&h->frames[frame&(UR_WINDOW-1)];
    if(!f->valid || f->frame!=frame){if(!create)return 0;memset(f,0,sizeof(*f));f->frame=frame;f->valid=1;}
    return f;
}
static int ur_pads_ok(const UrHistory *h,uint8_t mask,const uint8_t pads[8]) {
    unsigned s;
    if(mask&~h->all)return 0;
    for(s=0;s<4;s++)for(unsigned b=0;b<UR_STRIDE(h->width);b++)
        if((!(mask&(1u<<s)) || b>=h->width) && pads[UR_STRIDE(h->width)*s+b])return 0;
    return 1;
}
int ur_submit(UrHistory *h,uint32_t frame,const uint8_t pads[8]) {
    UrFrame *f;unsigned s;
    if(!h->local || !ur_pads_ok(h,h->local,pads) || !(f=ur_frame(h,frame,1)))return 0;
    for(s=0;s<4;s++)if((h->local&(1u<<s)) && ((f->mask&(1u<<s)) || f->pending) &&
        memcmp(f->pads+UR_STRIDE(h->width)*s,pads+UR_STRIDE(h->width)*s,UR_STRIDE(h->width)))return 0;
    for(s=0;s<4;s++)if(h->local&(1u<<s))memcpy(f->pads+UR_STRIDE(h->width)*s,pads+UR_STRIDE(h->width)*s,UR_STRIDE(h->width));
    if((f->mask&h->local)!=h->local)f->pending=1;
    return 1;
}
int ur_merge(UrHistory *h,uint32_t frame,uint8_t mask,const uint8_t pads[8]) {
    UrFrame *f;unsigned s;
    if(!ur_pads_ok(h,mask,pads) || !(f=ur_frame(h,frame,1)))return 0;
    for(s=0;s<4;s++)if((mask&(1u<<s)) && ((f->mask&(1u<<s)) || (f->pending&&(h->local&(1u<<s)))) &&
        memcmp(f->pads+UR_STRIDE(h->width)*s,pads+UR_STRIDE(h->width)*s,UR_STRIDE(h->width)))return 0;
    for(s=0;s<4;s++)if(mask&(1u<<s))memcpy(f->pads+UR_STRIDE(h->width)*s,pads+UR_STRIDE(h->width)*s,UR_STRIDE(h->width));
    f->mask|=mask;if((mask&h->local)==h->local)f->pending=0;return 1;
}
int ur_release(UrHistory *h,uint32_t next) {
    uint32_t f;
    if(next<h->floor || next-h->floor>UR_WINDOW)return 0;
    for(f=h->floor;f<next;f++){UrFrame *p=ur_frame(h,f,0);if(!p || p->mask!=h->all)return 0;}
    h->floor=next;return 1;
}
uint8_t ur_samples(const UrHistory *h,uint32_t frame,uint8_t mask,uint8_t pads[32],int redundant) {
    unsigned count=0,limit=redundant && h->width<=2?UR_SAMPLES:1;
    memset(pads,0,32);
    do{
        const UrFrame *f=&h->frames[frame&(UR_WINDOW-1)];
        /* Released slots can still supply redundancy until overwritten. Never
         * cross a gap or mistake a reused ring slot for an older frame. */
        if(!f->valid || f->frame!=frame || (f->mask&mask)!=mask)break;
        for(unsigned s=0;s<4;s++)if(mask&(1u<<s))memcpy(pads+UR_PAD_BYTES(h->width)*count+UR_STRIDE(h->width)*s,f->pads+UR_STRIDE(h->width)*s,UR_STRIDE(h->width));
        count++;
    }while(frame-- && count<limit);
    return h->width | ((count?count-1:0)<<6);
}
int ur_merge_packet(UrHistory *h,const uint8_t *packet) {
    uint32_t frame=ur_get32(packet+18);
    for(unsigned i=0;i<=(packet[23]>>6);i++,frame--){
        if(frame<h->floor || frame-h->floor>=UR_WINDOW)continue;
        if(!ur_merge(h,frame,packet[22]&15,packet+24+UR_PAD_BYTES(h->width)*i))return 0;
    }
    return 1;
}
