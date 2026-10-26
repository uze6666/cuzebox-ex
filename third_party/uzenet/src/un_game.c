#include "un_game.h"
#include <string.h>
void ug_reset(UrGame *g){memset(g,0,sizeof(*g));}
static uint32_t expand(uint32_t base,uint8_t low){return base+(uint8_t)(low-(uint8_t)base);}
static uint32_t ready_before(UrClient *c){
    uint32_t f=c->released;
    while(f<c->next){UrFrame *v=ur_frame(&c->history,f,0);if(!v || v->mask!=c->history.all)break;f++;}
    return f;
}
static uint8_t record(UrClient *c,uint32_t frame,uint8_t *p){
    UrFrame *v=ur_frame(&c->history,frame,0);uint8_t mask=v?v->mask:0,used=4;
    p[0]=(uint8_t)frame;p[1]=mask|(uo_pending(c)?UN_FRAME_OOB:0);p[2]=(uint8_t)c->next;p[3]=(uint8_t)c->history.floor;
    for(unsigned s=0;s<4;s++)if((c->history.all&~c->history.local)&(1u<<s)){
        memset(p+used,0,c->history.width);
        if(mask&(1u<<s))memcpy(p+used,v->pads+UR_STRIDE(c->history.width)*s,c->history.width);
        used+=c->history.width;
    }
    return used;
}
uint8_t ug_command(UrGame *g,UrClient *c,uint8_t type,const uint8_t *p,uint8_t n,uint32_t now){
    uint8_t reply[24]={0},used=0,error=3;
    if(type!=UN_PLAY && type!=UN_TICK && type!=UN_READ && type!=UN_CREDIT && type!=UN_LINK &&
       (type<UN_OOB_WRITE || type>UN_OOB_STATUS))return 0;
    if(c->state!=UC_READY){error=2;goto failed;}
    if(type==UN_OOB_WRITE){
        uint8_t result=uo_write(c,p,n,now);if(!result)goto failed;
        reply[0]=p[0];reply[1]=p[1];reply[2]=result==2;
        reply[3]=c->oob.uploaded;reply[4]=c->oob.pending;used=5;
    }else if(type==UN_OOB_READ && n==3){
        used=uo_read(c,p,reply);if(!used)goto failed;
    }else if(type==UN_OOB_ACK && n==5){
        if(!uo_ack(c,p[0],ur_get32(p+1)))goto failed;
        memcpy(reply,p,5);used=5;
    }else if(type==UN_OOB_STATUS && !n){
        reply[0]=(uint8_t)c->oob.id;reply[1]=c->oob.id>>8;
        reply[2]=!c->oob.id?0:c->oob.uploaded<c->oob.total?1:c->oob.pending?2:3;
        reply[3]=c->oob.uploaded;reply[4]=c->oob.total;reply[5]=c->oob.pending;
        reply[6]=UO_MESSAGE_MAX;reply[7]=UO_WRITE_MAX;reply[8]=UO_READ_MAX;reply[9]=uo_pending(c);used=10;
    }else if(type==UN_PLAY && n==5 && p[0]<=UG_PUSH){
        uint32_t base=ur_get32(p+1);
        if(!(g->active && g->base==base && g->mode==p[0])){
            if(g->active || base!=c->released)goto failed;
            ug_reset(g);g->active=1;g->base=base;g->mode=p[0];
            up_begin(c,base,now);
        }
        memcpy(reply,p,5);reply[5]=UR_WINDOW;used=6;
    }else if(type==UN_TICK && g->active && n==3+__builtin_popcount(c->history.local)*c->history.width){
        uint32_t frame=expand(c->history.floor,p[0]),consume=expand(c->released,p[1]),want=expand(c->released,p[2]);
        uint8_t pads[UR_PAD_MAX]={0},at=3;
        if(frame-c->history.floor>=UR_WINDOW || frame<c->released || frame>c->next ||
           consume>frame || consume-c->released>UR_WINDOW || want<consume || want-c->history.floor>=UR_WINDOW ||
           !up_valid(c,want) || frame<want || frame-want>UP_INPUT_LEAD)goto failed;
        /* Validate the entire release before changing any state. */
        for(uint32_t f=c->released;f<consume;f++){UrFrame *v=ur_frame(&c->history,f,0);if(!v || v->mask!=c->history.all)goto failed;}
        for(unsigned s=0;s<4;s++)if(c->history.local&(1u<<s)){memcpy(pads+UR_STRIDE(c->history.width)*s,p+at,c->history.width);at+=c->history.width;}
        if(!uc_submit(c,frame,pads))goto failed;
        for(uint32_t f=c->released;f<consume;f++)g->delivered[f&(UR_WINDOW-1)]=0;
        (void)uc_release(c,consume);(void)uc_fetch(c,want);
        up_advance(c,want,now);
        used=record(c,want,reply);
        g->delivered[want&(UR_WINDOW-1)]=reply[1]&UN_FRAME_ACTUAL_MASK&~c->history.local;
        if(up_run(c,now))reply[1]|=UN_FRAME_RUN;
        /* Pushed frames are advisory. Retry TICK for the same simulation
         * step, or use READ to re-fetch another retained frame after CRC loss. */
    }else if(type==UN_READ && n==1 && g->active){
        uint32_t want=expand(c->released,p[0]);
        if(want-c->history.floor>=UR_WINDOW || !uc_fetch(c,want))goto failed;
        used=record(c,want,reply); /* Read-ahead never grants or acknowledges a tick. */
    }else if(type==UN_CREDIT && n==2 && g->active && (g->mode&UG_PUSH)){
        if(p[0]==g->credit_id){if(p[1]!=g->last_grant)goto failed;}
        else{
            if(p[0]!=(uint8_t)(g->credit_id+1) || (unsigned)g->credit+p[1]>255)goto failed;
            g->credit_id=p[0];g->last_grant=p[1];g->credit+=p[1];
        }
        reply[0]=g->credit_id;reply[1]=g->credit;used=2;
    }else if(type==UN_LINK && n==2 && p[0]<=4 && p[1]>=54){
        UrLink *link=p[0]==4?&c->relay:&c->peers[p[0]].link;
        reply[0]=p[0];reply[1]=(p[0]==4?2:((ud_mask(c,now)&(1u<<p[0]))?1:(c->peers[p[0]].known?2:0)))|(link->samples?128:0);
        reply[2]=(uint8_t)link->rtt;reply[3]=link->rtt>>8;
        reply[4]=(uint8_t)link->jitter;reply[5]=link->jitter>>8;
        reply[6]=(uint8_t)c->retries;reply[7]=c->retries>>8;
        ur_put32(reply+8,c->next);ur_put32(reply+12,link->next);ur_put32(reply+16,ready_before(c));
        for(uint32_t f=c->history.floor;f<c->next;f++){
            UrFrame *v=ur_frame(&c->history,f,0);
            if(v && v->pending)reply[20]++;
            if(f>=c->released && (!v || v->mask!=c->history.all))reply[21]++;
        }
        reply[22]=(uint8_t)c->tx_errors;reply[23]=c->tx_errors>>8;used=24;
    }else goto failed;
    un_send(type|0x80,reply,used);return 1;
failed:
    reply[0]=type;reply[1]=error;un_send(UN_ERROR,reply,2);return 1;
}
void ug_push(UrGame *g,UrClient *c){
    uint8_t reply[4+UR_PAD_MAX];
    if(!g->active || !(g->mode&UG_PUSH) || !g->credit || c->state!=UC_READY)return;
    for(unsigned i=0;i<UR_WINDOW;i++){
        uint32_t f=c->released+((g->scan+i)&(UR_WINDOW-1));
        if(f>=c->next)continue;
        uint8_t n=record(c,f,reply),mask=reply[1]&UN_FRAME_ACTUAL_MASK&~c->history.local;
        if(!mask || mask==g->delivered[f&(UR_WINDOW-1)])continue;
        uint8_t bytes=un_encoded_size(UN_FRAME,reply,n);
        if(bytes>g->credit)return;
        g->credit-=bytes;g->delivered[f&(UR_WINDOW-1)]=mask;g->scan=(uint8_t)(f-c->released+1);
        un_send(UN_FRAME,reply,n);return;
    }
}
void ug_recover(UrGame *g,UrClient *c){
    /* Called even while UART is paused or credits are exhausted. */
    if(!g->active || c->state!=UC_READY)return;
    uint32_t missing=ready_before(c);
    if(missing<c->next)(void)uc_fetch(c,missing);
}
