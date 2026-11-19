#ifndef GROM_DISK_H
#define GROM_DISK_H
#include <stdint.h>


#define GROM_MAJOR 0
#define GROM_MINOR 1

#define WIPE_LIST_MAX 64




enum grom_slot_flags {
  GS_SPLIT = 1,            // Split between an external storage (sd card) and internel storage
  GS_USERDATA = 2,         // The userdata portion of a split slot. 
  GS_EXTERNAL_STORAGE = 4, // The externel storage portion of a split slot
};

struct grom_parition {
  char name[128];          // The name of the partition, typically a name like 'userdata' or 'metadata'
                           // but a mount point (as it apears in the fstab) also works as a fallback
                        
  uint8_t extent_count;    // The count of extents this requires, the begining extent is where (sequentially) it started at
                           // in the slot extents array, so if we are the first extent with count 2 that is index zero,
                           // the next slot will live at extent index 2.
};


// What is encrypted inside of a 
struct grom_slot_payload{
  uint32_t flags; // Of type grom_slot_flags  

  char label[32];
  uint8_t  xts_master_key[64];

  uint16_t wipe_count;
  uint16_t wipe_list[WIPE_LIST_MAX];


  uint16_t parition_count; // Size of paritions array
  
  uint64_t extents[64][2];


  struct grom_parition paritions[]; // Tells which extents each partition lives in, dynamically sized based on partition_count
} __attribute__((packed));

/*
 * Exact 4096-byte on-disk representation of one keyslot.
 * Indistinguishable from random noise without the passphrase.
 */
struct grom_on_disk_keyslot {
    uint8_t salt[32];                      // Salt bestw
    uint8_t nonce[24];                     // XChaCha20-Poly1305 nonce
    uint8_t mac[16];                       // Poly1305 authentication tag
    uint8_t ciphertext[4096 - (32+24+16)];    // Encrypted struct grom_slot_payload 
} __attribute__((packed));

#define SLOT_PAYLOAD_SIZE (sizeof(((struct grom_on_disk_keyslot*)NULL)->ciphertext))


// This is the file format for the beginning of a grom formatted disk.
// all numbers are little endian
struct  grom_userdata  {
  char __magic[9]; // GROM_MAGIC
  uint8_t disk_version_major;
  uint8_t disk_version_minor;

  // Could be 16, 64, 128
  uint16_t slot_count;

  // This salt is used for every slot/admin password on the disk. This
  // has the downside of meaning every slot can be cracked at once,
  // but has the upside of making unlocking the device not take ten
  // minutes. 
  uint8_t disk_salt[16];


  // This is the hash of the admin password.
  // NOTE: This is not encrypted, just the raw hash. This is because the admin
  // password is not a true security measure, it just prevents low skilled attackers from wiping paritions.
  uint8_t admin_password_hash[32];
} __attribute__((packed));







int grom_create_userdata();
int search_device(const char* key, const char* value, int* major, int* minor);
int grom_hash(const char *password, const uint8_t salt[16], uint8_t out_key[32]);
int grypt_create_header(uint8_t sector[4096], const char* admin_password, uint16_t slot_count);
int gcrypt_format(const char* block_device, const char* admin_password, uint16_t slot_count);
int gcrypt_load_header(int fd, struct grom_userdata* data);

#endif
