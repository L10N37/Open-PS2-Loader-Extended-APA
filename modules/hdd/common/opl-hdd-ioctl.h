#ifndef OPL_HDD_IOCTL_H
#define OPL_HDD_IOCTL_H

// Commands and structures for XATAD.IRX / XHDD64.IRX
#define ATA_DEVCTL_IS_48BIT              0x6840
#define ATA_DEVCTL_SET_TRANSFER_MODE     0x6841
#define ATA_DEVCTL_READ_PARTITION_SECTOR 0x6842
#define ATA_DEVCTL_GET_HIGHEST_UDMA_MODE 0x6843
#define ATA_DEVCTL_READ_SECTORS64        0x6844

#define ATA_XFER_MODE_MDMA 0x20
#define ATA_XFER_MODE_UDMA 0x40

// structs for DEVCTL commands

typedef struct
{
    int type;
    int mode;
} hddAtaSetMode_t;

// 64-bit raw read request used only by OPL's bank scanner. Split the LBA into
// two u32 words so the IOP-side ABI has no compiler-dependent 64-bit padding.
typedef struct
{
    u32 lba_lo;
    u32 lba_hi;
    u32 size; // number of 512-byte sectors
} hddAtaRead64_t;

#endif
