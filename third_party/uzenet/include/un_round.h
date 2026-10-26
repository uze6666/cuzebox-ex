#ifndef UN_ROUND_H
#define UN_ROUND_H
#include <stdint.h>

/* Server-side round admission policy. Call only after authenticating a peer
 * and deriving its seat mask from the Room lease, never from request data.
 * This module does not allocate AVR memory or enable a wire profile. */
#define URR_DESCRIPTOR_SIZE 16
enum { URR_SETUP=1, URR_PLAY, URR_FAILED };
enum { URR_REJECT=0, URR_ACCEPT, URR_TRANSITION, URR_RETRY };
typedef struct {
    uint32_t base, boundary;
    uint16_t epoch;
    uint8_t active, bound, finished, ready, phase, descriptor_valid;
    uint8_t previous_active;
    uint8_t descriptor[URR_DESCRIPTOR_SIZE];
} UrRound;

/* Seat 1 is the fixed host. Unclaimed seats are deterministic game CPUs. */
void urr_init(UrRound *r);
/* Binding during play/setup reserves a seat for the NEXT admission only.
 * Losing an active participant terminates; losing a pending one does not. */
int urr_bound(UrRound *r, uint8_t bound);
/* Each active console finishes at the same next-frame boundary. The final
 * finish atomically freezes the new roster and discards 64 frames of timeline
 * address space, isolating delayed inputs from the previous round. */
int urr_finish(UrRound *r, uint8_t seats, uint16_t epoch, uint32_t boundary);
/* Host publishes immutable setup. Every console must echo those exact bytes
 * only AFTER loading; no hash collision can count as setup agreement. */
int urr_describe(UrRound *r, uint8_t seats, uint16_t epoch,
                 const uint8_t descriptor[URR_DESCRIPTOR_SIZE]);
int urr_loaded(UrRound *r, uint8_t seats, uint16_t epoch,
               const uint8_t descriptor[URR_DESCRIPTOR_SIZE]);
#endif
