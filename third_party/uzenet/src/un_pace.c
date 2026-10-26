#include "un_client.h"
#include <string.h>
void up_begin(UrClient *c,uint32_t frame,uint32_t now){
    UrPace *p=&c->pace;memset(p,0,sizeof(*p));p->active=1;p->frame=frame;
    p->born=p->advanced_at=now;p->correct_at=now-250;p->last_tx=now-5;p->period=17*256;
    for(unsigned i=0;i<4;i++)p->peer[i].send_at=now-UP_INTERVAL_MS;
}
int up_valid(const UrClient *c,uint32_t frame){
    const UrPace *p=&c->pace;
    return p->active && (frame==p->frame || (p->granted && frame==p->frame+1));
}
void up_advance(UrClient *c,uint32_t frame,uint32_t now){
    UrPace *p=&c->pace;
    if(frame==p->frame)return;
    uint32_t elapsed=now-p->advanced_at;
    if(elapsed && elapsed<=100)p->period+=((int32_t)(elapsed*256)-p->period)/16;
    p->advanced_at=now;p->frame=frame;p->granted=0;
}
uint8_t up_run(UrClient *c,uint32_t now){
    UrPace *p=&c->pace;uint8_t covered=c->history.local;int ahead=0;
    if(!p->active || c->state!=UC_READY)return 0;
    /* A delivered RUN is irrevocable for this frame, including CRC retries. */
    if(p->granted)return 1;
    for(unsigned i=0;i<4;i++){
        UpPeer *v=&p->peer[i];if(!v->mask)continue;
        if(now-v->seen_at>UP_STALE_MS)return 0;
        covered|=v->mask;
        if(p->frame>=v->frame && p->frame-v->frame>=UP_HARD_LEAD)return 0;
        if(v->samples>=4 && v->lead>2*256)ahead=1;
    }
    if(covered!=c->history.all){
        if(now-p->born>=10000)uc_fail(c,14); /* no pacing-capable partner */
        return 0;
    }
    if(p->pause_ms){
        if(now-p->pause_at<p->pause_ms)return 0;
        p->pause_ms=0;
    }
    if(ahead && now-p->correct_at>=250){
        p->pause_ms=(p->period+128)/256;if(!p->pause_ms)p->pause_ms=1;
        p->pause_at=p->correct_at=now;
        /* Account for this one-frame correction rather than repeatedly
         * reacting to the same filtered observation before another probe. */
        for(unsigned i=0;i<4;i++)if(p->peer[i].mask && p->peer[i].lead>-63*256)p->peer[i].lead-=256;
        return 0;
    }
    p->granted=1;return 1;
}
int up_receive(UrClient *c,unsigned peer,uint8_t mask,int response,
               uint32_t frame,const uint8_t payload[8],uint32_t now){
    UrPace *p=&c->pace;uint32_t token=ur_get32(payload),echo=ur_get32(payload+4);
    if(!p->active || c->state!=UC_READY || peer>=4 || peer==c->peer || !token ||
       frame>=0xffffffc0UL || !mask || (mask&~c->history.all) || (mask&c->history.local))return 0;
    UpPeer *v=&p->peer[peer];
    if(v->mask && mask!=v->mask)return 0;
    for(unsigned i=0;i<4;i++)if(i!=peer && (p->peer[i].mask&mask))return 0;
    if(response){
        if(token!=v->token || echo!=v->sent_frame || now-v->sent_at>UP_STALE_MS)return 0;
        if(!v->retries){
            uint32_t elapsed=now-v->sent_at;
            uint16_t rtt=elapsed>65535?65535:elapsed;
            if(!v->samples){v->rtt=rtt;v->jitter=0;}
            else{
                int32_t delta=(int32_t)rtt-v->rtt;
                v->rtt+=delta/8;if(delta<0)delta=-delta;
                v->jitter+=(delta-v->jitter)/4;
            }
            /* Compare remote progress to the midpoint of our own progress
             * over this round trip. No synchronized clocks or assumed Hz. */
            int64_t lead=((int64_t)v->sent_frame+p->frame-2*(int64_t)frame)*128;
            if(lead>63*256)lead=63*256;
            if(lead<-63*256)lead=-63*256;
            if(!v->samples)v->lead=(int16_t)lead;
            else v->lead+=(int32_t)(lead-v->lead)/8;
            if(v->samples<255)v->samples++;
        }
        v->token=0;
    }else{
        if(echo)return 0;
        v->reply_token=token;v->reply_echo=frame;v->reply_pending=1;
    }
    v->mask=mask;
    if(frame>=v->frame){v->frame=frame;v->seen_at=now;}
    return 1;
}
int up_next(UrClient *c,uint32_t now,uint8_t packet[48],uint8_t destination[6]){
    if(c->protocol==UR_PROFILE_LOBBY || (c->protocol==UR_PROFILE_READYSTART && c->lobby.phase!=2))return 0;
    UrPace *p=&c->pace;
    if(!p->active || c->state!=UC_READY || now-p->last_tx<5)return 0;
    uint8_t covered=c->history.local;
    for(unsigned i=0;i<4;i++)covered|=p->peer[i].mask;
    for(unsigned pass=0;pass<2;pass++)for(unsigned k=0;k<4;k++){
        unsigned i=(p->scan+k)&3;UpPeer *v=&p->peer[i];
        if(i==c->peer)continue;
        int response=v->reply_pending;
        if(pass==0?!response:(response || now-v->send_at<UP_INTERVAL_MS ||
           (covered==c->history.all && !v->mask)))continue;
        uint8_t payload[8]={0};uint32_t frame;
        if(response){
            ur_put32(payload,v->reply_token);ur_put32(payload+4,v->reply_echo);
            frame=p->frame;v->reply_pending=0;
        }else{
            if(!v->token || now-v->sent_at>=UP_STALE_MS){
                if(p->serial==UINT32_MAX){uc_fail(c,9);return 0;}
                v->token=++p->serial;v->sent_frame=p->frame;v->sent_at=now;v->retries=0;
            }else if(v->retries<255)v->retries++;
            v->send_at=now;frame=v->sent_frame;ur_put32(payload,v->token);
        }
        p->last_tx=now;p->scan=(i+1)&3;
        if(ud_mask(c,now)&(1u<<i)){
            UrPeer *remote=&c->peers[i];
            if(remote->seq==UINT32_MAX){uc_fail(c,9);return 0;}
            memcpy(destination,remote->address,6);
            return ur_seal(packet,remote->key,2+c->peer,c->match,c->peer,
                response?UD_PACE_REPLY:UD_PACE,++remote->seq,frame,c->history.local,c->history.width,payload);
        }
        if(c->seq==UINT32_MAX){uc_fail(c,9);return 0;}
        memcpy(destination,c->endpoint,6);
        return ur_seal(packet,c->key,0,c->match,c->peer,response?UR_PACE_REPLY:UR_PACE,
            ++c->seq,frame,1u<<i,c->history.width,payload);
    }
    return 0;
}
