/* Local firmware endpoint. The authenticated WebSocket gateway owns UDP MACs. */
#include <emscripten.h>
#include <string.h>
#include "un_endpoint.h"

static UnEndpoint endpoint;
static UrGame game;
static UrClient client;
static UnRx rx;
static uint8_t incoming[UR_PACKET_MAX];
static unsigned connected, serviced;
static uint32_t last_service;
extern int web_uart_push(unsigned value);

EM_JS(void, web_packet_send, (unsigned kind,const uint8_t *p,unsigned n), {
    if(Module.uzenetPacket) Module.uzenetPacket(kind,HEAPU8.slice(p,p+n));
});
EM_JS(void, web_uart_reply, (unsigned value), {
    if(Module.uzenetReply) Module.uzenetReply(value);
});
EM_JS(void, web_nonce, (uint8_t *p), {
    HEAPU8.set(crypto.getRandomValues(new Uint8Array(4)),p);
});
static uint32_t now_ms(void){return (uint32_t)emscripten_get_now();}

/* The authenticated browser connection supplies its display name. Preserve
 * complete UTF-8 characters within the firmware profile's 24-byte limit. */
EM_JS(int, web_profile_name, (uint8_t *p), {
    const encoder=new TextEncoder();let bytes=[];
    for(const character of String(Module.uzenetPlayerName || '')){
        const part=encoder.encode(character);
        if(bytes.length+part.length>24)break;
        bytes.push(...part);
    }
    HEAPU8.set(bytes,p);return bytes.length || -1;
});
int us_profile_read(uint8_t local,uint8_t text[US_NAME_MAX]){
    return local?-1:web_profile_name(text);
}

/* These records never go directly to UDP. The gateway verifies incoming MACs
 * and seals outgoing records with this connection's server-held traffic key.
 * Reject a non-redacted key so this adapter cannot masquerade as native crypto. */
int ur_mac(const uint8_t key[32],uint8_t domain,const uint8_t *body,size_t n,uint8_t tag[16]){
    (void)body;
    if(n>UR_PACKET_MAX-16 || (domain!=0 && domain!=1 && domain!=6 && domain!=7))return 0;
    for(unsigned i=0;i<32;i++)if(key[i])return 0;
    memset(tag,0,16);return 1;
}
void un_put(uint8_t value){
    if(!web_uart_push(value)){uc_fail(&client,7);return;}
    web_uart_reply(value);
}
uint8_t ue_start(const uint8_t config[19],uint32_t now){
    uint8_t request[UR_REQUEST],nonce[4];web_nonce(nonce);
    if(!uc_begin(&client,config,nonce,now,request))return UE_BAD_REQUEST;
    web_packet_send(1,request,sizeof(request));return 0;
}
void web_endpoint_connect(unsigned enabled){
    connected=enabled;serviced=0;
    ue_reset(&endpoint);ug_reset(&game);uc_reset(&client);memset(&rx,0,sizeof(rx));
    memset(incoming,0,sizeof(incoming));
}
EMSCRIPTEN_KEEPALIVE void web_net_tick(void);
void web_endpoint_byte(uint8_t value){
    if(!connected)return;
    unsigned n=un_receive(&rx,value);
    if(n && rx.data[0]==UE_REQUEST){
        ue_command(&endpoint,&game,&client,rx.data+1,n-1,now_ms());
        web_net_tick();
    }
}
EMSCRIPTEN_KEEPALIVE void web_net_tick(void){
    uint32_t now=now_ms();
    if(!connected)return;
    if(!serviced || now!=last_service){
        serviced=1;last_service=now;
        ue_tick(&endpoint,&client,now);
    }
    uint8_t packet[UR_PACKET_MAX],destination[6];
    if(uc_next(&client,now,packet))web_packet_send(2,packet,UR_SIZE(packet));
    if(up_next(&client,now,packet,destination))web_packet_send(2,packet,UR_SIZE(packet));
    if(uo_next(&client,now,packet,destination))web_packet_send(2,packet,UR_SIZE(packet));
    ue_service(&endpoint,&game,&client,now);
}
EMSCRIPTEN_KEEPALIVE uint8_t *web_net_buffer(void){return incoming;}
EMSCRIPTEN_KEEPALIVE int web_net_receive(unsigned kind,unsigned n){
    if(!connected)return 0;
    if(kind==1 && n==UR_TICKET){
        if(!uc_ticket(&client,incoming,now_ms()))return 0;
    }else if(kind==2 && n>=UR_PACKET && n<=UR_PACKET_MAX){
        (void)uc_receive(&client,incoming,n,now_ms());
    }else return 0;
    web_net_tick();return 1;
}
/* Local diagnostics: agreed input delay, endpoint frame, backend state. */
EMSCRIPTEN_KEEPALIVE unsigned web_net_delay(void){return endpoint.delay;}
EMSCRIPTEN_KEEPALIVE unsigned web_net_frame(void){return endpoint.frame;}
EMSCRIPTEN_KEEPALIVE unsigned web_net_state(void){return client.state;}
