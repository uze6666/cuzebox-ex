#ifndef UN_RELAY_H
#define UN_RELAY_H
#include <stdint.h>
#include <stddef.h>
#define UR_WINDOW 64
#define UR_PACKET 48
#define UR_BODY 32
#define UR_REDUNDANT 0x80 /* CONFIG width flag; all room peers opt in */
#define UR_LOBBY 0x20 /* Opt-in owner-snapshot lobby; one human per console. */
#define UR_PROFILE_LOBBY 8
#define UR_DROPIN 0x10 /* Lockstep with a frozen host state hand-off. */
#define UR_PROFILE_DROPIN 9
/* Two- and four-seat drop-in select 1..4 input bytes per seat. Four seats
 * additionally opt into repeated admissions; the original wire format stays. */
#define UR_DROPIN_CONFIG(c) ((c)[16]==1 && ((c)[17]==2 || (c)[17]==4) && \
    ((c)[18]&UR_WIDTH_FLAGS)==UR_DROPIN && ((c)[18]&15)>=1 && ((c)[18]&15)<=4)
#define UR_READYSTART (UR_LOBBY|UR_DROPIN) /* Initial ready lobby, then frozen lockstep. */
#define UR_PROFILE_READYSTART 10
#define UR_PROFILE_LIVE 11
#define UR_IS_LIVE(flags) ((flags)==(UR_LOBBY|2))
#define UR_IS_READYSTART(flags) (((flags)&UR_READYSTART)==UR_READYSTART)
#define UR_ADMIT 14
#define UR_LOBBY_REQUEST 13
#define UR_ROSTER 0x8c
#define UR_WIDTH_FLAGS (UR_REDUNDANT|UR_LOBBY|UR_DROPIN)
#define UR_SAMPLES 4
#define UR_PACKET_MAX (UR_PACKET + 8 * (UR_SAMPLES - 1))
/* Preserve the two-byte seat stride for existing games. Wide inputs use
 * four-byte seats only in input/frame datagrams; control/OOB stay unchanged. */
#define UR_PAD_MAX 16
#define UR_STRIDE(w) (((w)&63)>2 ? 4u : 2u)
#define UR_PAD_BYTES(w) (4u * UR_STRIDE(w))
#define UR_IS_INPUT(t) ((t)==UR_INPUT || (t)==UR_INPUT_DIRECT || (t)==UR_FRAME || (t)==UD_INPUT)
#define UR_PAYLOAD(t,w) ((UR_IS_INPUT(t) ? UR_PAD_BYTES(w) : 8u) * (1u+((w)>>6)))
#define UR_SIZE(p) (40u + UR_PAYLOAD((p)[4],(p)[23]))
#define UR_TICKET 68
#define UR_CONFIG 19
#define UR_REQUEST 25
#define UR_OPEN 0x79
#define UR_CLOSE 0x7a
#define UR_PEER_INFO 0x7b
#define UR_BOT_LIST 0x7c /* Pinned server bot identities only; no public discovery. */
#define UR_BOT_OPEN 0x7d /* match[8], then the normal 25-byte OPEN request. */
#define UR_BOT_LIST_MAX 32
#define UR_BOT_LIST_HEADER 8
#define UR_BOT_LIST_RECORD 30
#define UR_INPUT_DIRECT 8
#define UD_PUNCH 0x40
#define UD_PONG 0x41
#define UD_INPUT 0x42
#define UD_PACE 0x43
#define UD_PACE_REPLY 0x44
#define UR_PACE 9
#define UR_PACE_REPLY 10
#define UR_PACE_FROM 0x88
#define UR_PACE_REPLY_FROM 0x89
#define UR_OOB 11
#define UR_OOB_ACK 12
#define UR_OOB_FROM 0x8a
#define UR_OOB_ACK_FROM 0x8b
#define UD_OOB 0x45
#define UD_OOB_ACK 0x46
#define UR_IS_OOB(t) ((t)==UR_OOB || (t)==UR_OOB_ACK || (t)==UR_OOB_FROM || (t)==UR_OOB_ACK_FROM || (t)==UD_OOB || (t)==UD_OOB_ACK)
#define UR_IS_OOB_DATA(t) ((t)==UR_OOB || (t)==UR_OOB_FROM || (t)==UD_OOB)
#define UR_PROFILE 6
#define UR_PROFILE_REDUNDANT 7
enum {UR_HELLO=1,UR_CONFIRM,UR_INPUT,UR_FETCH,UR_RELEASE,UR_PING,UR_LEAVE,
      UR_CHALLENGE=0x81,UR_BOUND,UR_READY,UR_FRAME,UR_ERROR,UR_END,UR_RELEASED};
typedef struct {uint32_t high;uint64_t bits;} UrReplay;
typedef struct {uint32_t frame,sent;uint8_t pads[UR_PAD_MAX],mask,valid,pending,transmitted;} UrFrame;
typedef struct {UrFrame frames[UR_WINDOW];uint32_t floor;uint8_t all,local,width;} UrHistory;
uint32_t ur_get32(const uint8_t *p);
void ur_put32(uint8_t *p,uint32_t n);
/* Implement with the platform crypto library. Domain 0=C->S, 1=S->C. */
int ur_mac(const uint8_t key[32],uint8_t domain,const uint8_t *body,size_t n,uint8_t tag[16]);
/* Output needs UR_SIZE bytes. Input pads use UR_PAD_BYTES(width) per
 * sample; control/OOB pads retain eight bytes per encoded sample. */
int ur_seal(uint8_t *p,const uint8_t key[32],uint8_t domain,
            const uint8_t match[8],uint8_t peer,uint8_t type,uint32_t seq,
            uint32_t frame,uint8_t mask,uint8_t width,const uint8_t pads[8]);
int ur_open(const uint8_t *p,size_t n,const uint8_t key[32],uint8_t domain,
            const uint8_t match[8],UrReplay *replay);
int ur_replay(UrReplay *r,uint32_t seq);
int ur_history_init(UrHistory *h,uint8_t all,uint8_t local,uint8_t width);
UrFrame *ur_frame(UrHistory *h,uint32_t frame,int create);
/* local submit / authenticated merge: immutable once seen, never invent input. */
int ur_submit(UrHistory *h,uint32_t frame,const uint8_t pads[8]);
int ur_merge(UrHistory *h,uint32_t frame,uint8_t mask,const uint8_t pads[8]);
int ur_release(UrHistory *h,uint32_t next);
/* Gather current and up to three consecutive earlier actual samples. The
 * returned width encodes count-1 in bits 7:6. Caller provides 32 pad bytes. */
uint8_t ur_samples(const UrHistory *h,uint32_t frame,uint8_t mask,uint8_t pads[32],int redundant);
int ur_merge_packet(UrHistory *h,const uint8_t *packet);
#endif
