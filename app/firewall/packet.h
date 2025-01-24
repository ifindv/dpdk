#ifndef _M_PACKET_H_
#define _M_PACKET_H_

typedef struct {
  uint8_t proto;
  uint32_t sip;
  uint32_t dip;
  uint16_t sp;
  uint16_t dp;
} ip4_tuple_t;

typedef struct {
  uint8_t proto;
  uint32_t sip[4];
  uint32_t dip[4];
  uint16_t sp;
  uint16_t dp;
} ip6_tuple_t;

// must align to 8 bytes
#pragma pack(1)

typedef struct {
  uint16_t port_in;
  uint16_t port_out;
  uint32_t ptype;
  uint32_t flags;
  uint16_t queue_id;    // which queue the packet come from (also send to)

  uint8_t smac[6];      // source mac
  uint8_t dmac[6];      // destination mac

  bool is_v4;
  union {
    ip4_tuple_t v4;
    ip6_tuple_t v6;
  } tuple;

  uint8_t reserved[189];
} packet_t;

#pragma pack()

#endif