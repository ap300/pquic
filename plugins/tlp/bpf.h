#include "picoquic_internal.h"

typedef struct {
    /* TLP Data */
    uint8_t tlp_nb;
    uint64_t tlp_time;
    uint64_t tlp_packet_send_time; /* Detect if the TLP probe changed or not */
    uint64_t tlp_last_asked; /* Don't enter infinite loops... */
    uint8_t print;
} bpf_data;