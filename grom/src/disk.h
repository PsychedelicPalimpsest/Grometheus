#ifndef GROM_DISK_H
#define GROM_DISK_H
#include <stdint.h>

#define GROM_MAJOR 0
#define GROM_MINOR 0

#define WIPE_LIST_MAX 64



int grom_create_userdata();


// 14 byte magic value for grom paritioned disks (including sd cards)
const char* GROM_MAGIC = "GromCrypt";


// What is encrypted inside of a 
struct grom_slot_payload{
  uint32_t flags;

  char label[32];
  uint8_t  xts_master_key[64];

  uint16_t wipe_count;
  uint16_t wipe_list[WIPE_LIST_MAX];
  
} __attribute__((packed)) ;

/*
 * Exact 4096-byte on-disk representation of one keyslot.
 * Indistinguishable from random noise without the passphrase.
 */
struct grom_on_disk_keyslot {
    uint8_t salt[32];                  /* Argon2id salt (random per write) */
    uint8_t nonce[24];                 /* XChaCha20-Poly1305 nonce */
    uint8_t mac[16];                   /* Poly1305 authentication tag */
    uint8_t ciphertext[4096 - 64];     /* Encrypted struct grom_slot_payload */
} __attribute__((packed));


// This is the file format for the beginning of a grom formatted disk.
// Note: Nothing here should be important
struct  grom_userdata  {
  char __magic[9]; // GROM_MAGIC
  uint8_t disk_version_major;
  uint8_t disk_version_minor;

  // Could be 16, 64, 128
  uint16_t slot_count;
} __attribute__((packed));






#endif
