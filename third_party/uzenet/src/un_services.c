#include "un_client.h"
#include <string.h>
enum {US_HELLO=1,US_OFFER,US_NAME,US_CHECK,US_BARRIER,US_END};
static void failure(UrClient *c,uint8_t reason){uc_note_failure(c,2,reason,0);c->services.failure=reason;}
__attribute__((weak)) int us_profile_read(uint8_t local,uint8_t text[US_NAME_MAX]){
    (void)local;(void)text;return -1;
}
__attribute__((weak)) int us_profile_write(uint8_t local,const uint8_t *text,uint8_t n){
    (void)local;(void)text;(void)n;return 0;
}
/* Shared UART errors: 6 desync, 7 negotiation, 8 invalid peer metadata. */
static int text_ok(const uint8_t *p,unsigned n){
    for(unsigned i=0;i<n;){
        uint32_t code=p[i++];unsigned extra=0;uint32_t minimum=0;
        if(code<128){if(code<32 || code==127)return 0;continue;}
        if(code>=0xc2 && code<=0xdf){extra=1;minimum=0x80;code&=31;}
        else if(code>=0xe0 && code<=0xef){extra=2;minimum=0x800;code&=15;}
        else if(code>=0xf0 && code<=0xf4){extra=3;minimum=0x10000;code&=7;}
        else return 0;
        if(i+extra>n)return 0;
        while(extra--){uint8_t b=p[i++];if((b&0xc0)!=0x80)return 0;code=(code<<6)|(b&63);}
        if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff) ||
           (code>=0x80 && code<=0x9f))return 0;
    }
    return 1;
}
void us_begin(UrClient *c,const uint8_t policy[4],uint8_t delay,uint8_t window,uint32_t now){
    UrServices *s=&c->services;
    memset(s,0,sizeof(*s));s->active=1;s->born=now;s->window=window;
    s->flags=policy[0];s->minimum=policy[1];s->maximum=policy[2];s->tick=policy[3];
    s->ready=!(s->flags&US_AUTO_DELAY);s->delay=delay;
    s->known=c->history.local;s->seen=c->history.local;
    s->hello_send=!!s->flags;
    unsigned local=0;
    for(unsigned seat=0;seat<4;seat++)if(c->history.local&(1u<<seat)){
        int n=us_profile_read(local++,s->names[seat]);
        if(n==-2){failure(c,8);return;}
        if(n<0 || n>US_NAME_MAX || !text_ok(s->names[seat],n) || !n){
            memcpy(s->names[seat],"PLAYER 1",8);s->names[seat][7]='1'+seat;n=8;
        }
        s->length[seat]=n;s->dirty|=1u<<seat;
    }
}
static int upload(UrClient *c,const uint8_t *p,uint8_t n,int profile){
    UrServices *s=&c->services;
    if(n<3 || p[0]>=4 || (!profile && (!s->active || !(c->history.local&(1u<<p[0])))) ||
       p[2]>US_NAME_MAX || p[1]+n-3>p[2] || (n==3 && p[2]))return 0;
    if(p[1]){
        if(s->upload_seat!=p[0] || s->upload_total!=p[2] || s->upload_used!=p[1])return 0;
    }else{s->upload_seat=p[0];s->upload_total=p[2];s->upload_used=0;}
    memcpy(s->upload+p[1],p+3,n-3);s->upload_used+=n-3;
    if(s->upload_used==s->upload_total){
        if(!text_ok(s->upload,s->upload_total))return 0;
        if(profile)return us_profile_write(p[0],s->upload,s->upload_total)?1:-1;
        unsigned seat=p[0];s->length[seat]=s->upload_total;
        memset(s->names[seat],0,US_NAME_MAX);memcpy(s->names[seat],s->upload,s->upload_total);
        s->dirty|=1u<<seat;s->known|=1u<<seat;
    }
    return 1;
}
int us_name_set(UrClient *c,const uint8_t *p,uint8_t n){return upload(c,p,n,0);}
int us_profile_set(UrClient *c,const uint8_t *p,uint8_t n){return upload(c,p,n,1);}
static int checksum(UrClient *c,uint32_t frame,uint32_t hash,uint8_t mask,int local){
    UrServices *s=&c->services;UsCheck *v=0,*free_slot=0;
    for(unsigned i=0;i<US_CHECK_SLOTS;i++){
        UsCheck *p=s->check+i;
        if(p->mask && p->frame==frame){v=p;break;}
        if(!p->mask || (p->mask==c->history.all && !p->send && frame>p->frame))
            if(!free_slot || !p->mask || p->frame<free_slot->frame)free_slot=p;
    }
    if(!v){if(!free_slot)return 2;v=free_slot;memset(v,0,sizeof(*v));v->frame=frame;v->hash=hash;}
    /* Identical local submissions are harmless; changed local state is an
     * invalid command, not a replacement for an already announced checksum. */
    if(local && (v->mask&mask) && hash!=v->hash)return 0;
    if(v->mask && hash!=v->hash){
        if(!s->desync){uc_note_failure(c,3,6,v->mask|mask);s->check_frame=frame;s->check_hash=v->hash;s->check_known=1;}
        if(s->check_frame==frame)s->desync|=v->mask|mask;
    }
    if(local && !(v->mask&mask)){
        v->send=1;
        /* A remote checkpoint may arrive before the local simulation reaches
         * it. Broadcast our own hash, even after detecting a mismatch against
         * that remote reference, so the other consoles detect it too. The
         * first mismatch reference remains latched separately above. */
        v->hash=hash;
    }
    v->mask|=mask;
    if(!s->desync && (!s->check_known || frame>=s->check_frame)){
        s->check_frame=frame;s->check_hash=v->hash;s->check_known=1;
    }
    return 1;
}
int us_check(UrClient *c,uint32_t frame,uint32_t hash){
    return checksum(c,frame,hash,c->history.local,1);
}
void us_end(UrClient *c,uint8_t code,uint32_t now){
    UrServices *s=&c->services;
    if(code>US_ENDED)uc_note_failure(c,4,code,0);
    if(!s->end_code)s->end_at=now;
    if(code>s->end_code){s->end_code=code;s->end_send=1;}
    s->end_mask|=c->history.local;
}
uint8_t us_end_status(UrClient *c,uint32_t now){
    UrServices *s=&c->services;
    if(!s->end_code)return 0;
    if(s->end_result)return s->end_result;
    if(now-s->end_at>=US_END_MS){uc_note_failure(c,4,US_SESSION_TIMEOUT,0);return s->end_result=US_SESSION_TIMEOUT;}
    if(s->end_mask!=c->history.all || s->end_send || c->service.pending)return 0;
    for(unsigned i=0;i<4;i++)if(c->service.peer[i].ack_pending)return 0;
    return s->end_result=s->end_code;
}
int us_barrier(UrClient *c,uint8_t label,uint32_t frame,uint32_t now){
    UrServices *s=&c->services;UsBarrier *own=&s->barrier[c->peer];
    if(!s->active || s->end_code || frame>=0xffffffc0UL)return 0;
    if(!s->barrier_wait){
        if(own->generation==65535)return 0;
        own->generation++;own->label=label;own->frame=frame;own->mask=c->history.local;
        s->barrier_at=now;s->barrier_wait=s->barrier_send=1;
    }else if(own->label!=label || own->frame!=frame)return 0;
    uint8_t mask=own->mask;
    for(unsigned i=0;i<4;i++)if(i!=c->peer){
        UsBarrier *v=&s->barrier[i];
        if(v->generation!=own->generation)continue;
        if(v->label!=label || v->frame!=frame){us_end(c,US_SYNC_ERROR,now);return 1;}
        mask|=v->mask;
    }
    if(mask!=c->history.all || s->barrier_send)return 1;
    s->barrier_wait=2;return 2;
}
static int receive(UrClient *c,unsigned peer,const UoPeer *v,uint32_t now){
    UrServices *s=&c->services;const uint8_t *p=v->data;unsigned n=v->rx_total;
    uint8_t mask=v->mask;
    if(!n || v->rx_channel)return 0;
    if(p[0]==US_HELLO && n==6){
        if(p[1]!=US_AUTO_DELAY || p[2]>p[3] || p[3]>8 || p[4]<10 || p[4]>50 || p[5]>8)return 0;
        if(!s->flags || p[4]!=s->tick || p[5]!=s->window){failure(c,7);return 1;}
        if((s->seen&mask) && memcmp(s->policy[peer],p+1,5))return 0;
        memcpy(s->policy[peer],p+1,5);s->seen|=mask;
    }else if(p[0]==US_OFFER && n==2){
        if(!(s->seen&mask) || p[1]>8 || ((s->offered&mask) && s->offers[peer]!=p[1]))return 0;
        s->offers[peer]=p[1];s->offered|=mask;
    }else if(p[0]==US_NAME && n>=3){
        if(p[1]>=4 || !(mask&(1u<<p[1])) || p[2]>US_NAME_MAX || n!=3u+p[2] || !text_ok(p+3,p[2]))return 0;
        unsigned seat=p[1];memset(s->names[seat],0,US_NAME_MAX);
        memcpy(s->names[seat],p+3,p[2]);s->length[seat]=p[2];s->known|=1u<<seat;
    }else if(p[0]==US_CHECK && n==9){
        uint32_t frame=ur_get32(p+1);
        /* Bounded storage for nearby simulation checkpoints. Old retransmitted
         * messages cannot replace a newer verdict; future floods are invalid. */
        if(frame>=0xffffffc0UL || (frame>c->pace.frame && frame-c->pace.frame>UR_WINDOW))return 0;
        if(c->pace.frame>frame && c->pace.frame-frame>UR_WINDOW){
            /* Reliable metadata can outlive the input-history window. Finish
             * a retained local checkpoint, or its slot could stay busy forever
             * after a long loss burst. Ignore only untracked stale messages. */
            unsigned i=0;
            while(i<US_CHECK_SLOTS && (!s->check[i].mask || s->check[i].frame!=frame))i++;
            if(i==US_CHECK_SLOTS)return 1;
        }
        int result=checksum(c,frame,ur_get32(p+5),mask,0);
        if(result==2)return 2;
    }else if(p[0]==US_BARRIER && n==8){
        if(s->end_code)return 1;
        UsBarrier *own=&s->barrier[c->peer],*b=&s->barrier[peer];
        uint16_t generation=p[1]|(uint16_t)p[2]<<8;uint32_t frame=ur_get32(p+4);
        if(!generation || frame>=0xffffffc0UL)return 0;
        if(generation==b->generation)
            return b->frame==frame && b->label==p[3] && b->mask==mask;
        if(generation<b->generation)return 1; /* obsolete, never releases a later barrier */
        if(generation>own->generation){
            if(generation!=(unsigned)own->generation+1)return 0;
            if(s->barrier_wait)return 2; /* keep previous readiness until consumed */
        }else if(generation<own->generation)return 1;
        b->generation=generation;b->frame=frame;b->label=p[3];b->mask=mask;
    }else if(p[0]==US_END && n==2 && p[1]>=US_ENDED && p[1]<=US_SESSION_TIMEOUT){
        us_end(c,p[1],now);s->end_mask|=mask;
    }else return 0;
    return 1;
}
static void negotiate(UrClient *c,uint32_t now){
    UrServices *s=&c->services;
    if(!s->flags || s->ready || s->failure)return;
    if(now-s->born>=10000){failure(c,7);return;}
    if(s->seen!=c->history.all)return;
    unsigned lower=s->minimum,upper=s->maximum,rtt=0,jitter=0;uint8_t measured=c->history.local;
    for(unsigned i=0;i<4;i++)if(i!=c->peer && c->pace.peer[i].mask){
        const uint8_t *p=s->policy[i];UpPeer *v=&c->pace.peer[i];
        if(p[1]>lower)lower=p[1];
        if(p[2]<upper)upper=p[2];
        if(v->samples>=4 && now-v->seen_at<=UP_STALE_MS)measured|=v->mask;
        if(v->rtt>rtt)rtt=v->rtt;
        if(v->jitter>jitter)jitter=v->jitter;
    }
    if(lower>upper){failure(c,7);return;}
    if(!(s->offered&c->history.local)){
        if(now-s->born<1000 || measured!=c->history.all)return;
        /* One frame of scheduling margin; RTT/2 is an estimate, not a clock
         * synchronization claim. The agreed cap may still allow stalls. */
        unsigned delay=(((rtt+1)/2+2*jitter)+s->tick-1)/s->tick+1;
        if(delay<lower)delay=lower;
        if(delay>upper){delay=upper;s->clamped=1;}
        s->proposal=delay;s->offer_send=1;s->offered|=c->history.local;
    }
    if(s->offered!=c->history.all)return;
    unsigned chosen=s->proposal;
    for(unsigned i=0;i<4;i++)if(i!=c->peer && c->pace.peer[i].mask){
        if(s->offers[i]<lower || s->offers[i]>upper){failure(c,7);return;}
        if(s->offers[i]>chosen)chosen=s->offers[i];
    }
    s->delay=chosen;s->ready=1;
}
void us_tick(UrClient *c,uint32_t now){
    UrServices *s=&c->services;
    if(!s->active || c->state!=UC_READY)return;
    uint8_t pending=uo_internal_pending(c);
    for(unsigned i=0;i<4;i++)if(pending&(1u<<i)){
        UoPeer *v=&c->service.peer[i];int result=receive(c,i,v,now);
        if(result==2)continue;
        if(!result)failure(c,8);
        (void)uo_internal_ack(c,i,v->rx_id);
    }
    if(s->barrier_wait==1 && !s->end_code){
        UsBarrier *b=&s->barrier[c->peer];
        (void)us_barrier(c,b->label,b->frame,now);
        if(s->barrier_wait==1 && !s->end_code && now-s->barrier_at>=US_BARRIER_MS)
            us_end(c,US_SESSION_TIMEOUT,now);
    }
    for(unsigned i=0;i<4;i++)if(c->pace.peer[i].mask && now-c->pace.peer[i].seen_at>15000){
        uc_note_failure(c,4,US_SESSION_TIMEOUT,c->pace.peer[i].mask);
        us_end(c,US_SESSION_TIMEOUT,now);
    }
    if(!s->end_code)negotiate(c,now);
    uint8_t p[3+US_NAME_MAX],n=0;unsigned seat=4,index=US_CHECK_SLOTS;
    if(s->end_send){p[0]=US_END;p[1]=s->end_code;n=2;}
    else if(s->barrier_send && !s->end_code){
        UsBarrier *b=&s->barrier[c->peer];p[0]=US_BARRIER;
        p[1]=b->generation;p[2]=b->generation>>8;p[3]=b->label;ur_put32(p+4,b->frame);n=8;
    }else if(s->end_code){(void)us_end_status(c,now);return;}
    else if(s->hello_send){
        p[0]=US_HELLO;p[1]=s->flags;p[2]=s->minimum;p[3]=s->maximum;p[4]=s->tick;p[5]=s->window;n=6;
    }else if(s->offer_send){p[0]=US_OFFER;p[1]=s->proposal;n=2;}
    else{
        for(unsigned i=0;i<US_CHECK_SLOTS;i++)if(s->check[i].send){
            index=i;p[0]=US_CHECK;ur_put32(p+1,s->check[i].frame);ur_put32(p+5,s->check[i].hash);n=9;break;
        }
        if(!n)for(unsigned i=0;i<4;i++)if(s->dirty&(1u<<i)){
            seat=i;p[0]=US_NAME;p[1]=i;p[2]=s->length[i];memcpy(p+3,s->names[i],p[2]);n=3+p[2];break;
        }
    }
    if(n){
        uint8_t result=uo_internal_send(c,p,n,now);
        if(!result)failure(c,8);
        if(result==1){
            if(s->end_send)s->end_send=0;
            else if(s->barrier_send)s->barrier_send=0;
            else if(s->hello_send)s->hello_send=0;
            else if(s->offer_send)s->offer_send=0;
            else if(index<US_CHECK_SLOTS)s->check[index].send=0;
            else if(seat<4)s->dirty&=~(1u<<seat);
        }
    }
}
static void put16(uint8_t *p,uint32_t v){if(v>65535)v=65535;p[0]=v;p[1]=v>>8;}
uint8_t us_status(UrClient *c,uint32_t frame,uint8_t delay,uint32_t now,uint8_t p[22]){
    UrServices *s=&c->services;uint8_t missing=0,direct=0;unsigned rtt=0,jitter=0;
    UrFrame *f=ur_frame(&c->history,frame,0);
    missing=c->history.all&~(f?f->mask:0);
    uint8_t routes=ud_mask(c,now);
    for(unsigned i=0;i<4;i++){
        UpPeer *v=&c->pace.peer[i];
        if(routes&(1u<<i))direct|=c->peers[i].mask;
        if(v->mask && now-v->seen_at>UP_STALE_MS)missing|=v->mask;
        if(v->rtt>rtt)rtt=v->rtt;
        if(v->jitter>jitter)jitter=v->jitter;
    }
    p[0]=c->state;p[1]=c->error;
    p[2]=(s->active&&!s->ready?1:0)|(s->desync?2:0)|(s->clamped?4:0)|(s->failure?8:0);
    p[3]=c->history.local;p[4]=c->history.all;p[5]=missing;p[6]=direct;p[7]=delay;
    ur_put32(p+8,frame);put16(p+12,rtt);put16(p+14,jitter);
    put16(p+16,c->tx_errors);put16(p+18,c->retries);return 20;
}
uint8_t us_peer_status(UrClient *c,uint8_t seat,uint32_t now,uint8_t p[22]){
    if(seat>=4 || !(c->history.all&(1u<<seat)))return 0;
    unsigned peer=c->peer;uint8_t local=!!(c->history.local&(1u<<seat));
    if(!local)for(peer=0;peer<4;peer++)if(c->pace.peer[peer].mask&(1u<<seat))break;
    p[0]=seat;p[1]=peer;p[2]=local?8:0;p[3]=local?c->history.local:0;
    memset(p+4,0,10);
    if(peer<4 && !local){
        UpPeer *v=&c->pace.peer[peer];uint32_t age=now-v->seen_at;
        p[2]=2|((ud_mask(c,now)&(1u<<peer))?1:0)|(age>UP_STALE_MS?4:0);p[3]=v->mask;
        put16(p+4,age);put16(p+6,v->rtt);put16(p+8,v->jitter);ur_put32(p+10,v->frame);
    }
    return 14;
}
