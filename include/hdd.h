#ifndef __HDD_H
#define __HDD_H

#include <fileXio_rpc.h>
#include "opl-hdd-ioctl.h"

typedef struct
{
    u32 start;  // Sector address
    u32 length; // Sector count
} apa_sub_t;

typedef struct
{
    u8 unused;
    u8 sec;
    u8 min;
    u8 hour;
    u8 day;
    u8 month;
    u16 year;
} ps2time_t;

typedef struct
{
    u32 checksum;
    u32 magic; // APA_MAGIC
    u32 next;
    u32 prev;
    char id[APA_IDMAX];
    char rpwd[APA_PASSMAX];
    char fpwd[APA_PASSMAX];
    u32 start;
    u32 length;
    u16 type;
    u16 flags;
    u32 nsub;
    ps2time_t created;
    u32 main;
    u32 number;
    u32 modver;
    u32 pading1[7];
    char pading2[128];
    struct
    {
        char magic[32];
        u32 version;
        u32 nsector;
        ps2time_t created;
        u32 osdStart;
        u32 osdSize;
        char pading3[200];
    } mbr;
    apa_sub_t subs[APA_MAXSUB];
} apa_header_t;

#define PFS_INODE_MAX_BLOCKS 114

typedef struct
{
    u32 number;
    u16 subpart;
    u16 count;
} pfs_blockinfo_t;

typedef struct
{
    u32 checksum;
    u32 magic;
    pfs_blockinfo_t inode_block;
    pfs_blockinfo_t next_segment;
    pfs_blockinfo_t last_segment;
    pfs_blockinfo_t unused;
    pfs_blockinfo_t data[PFS_INODE_MAX_BLOCKS];
    u16 mode;
    u16 attr;
    u16 uid;
    u16 gid;
    ps2time_t atime;
    ps2time_t ctime;
    ps2time_t mtime;
    u64 size;
    u32 number_blocks;
    u32 number_data;
    u32 number_segdesg;
    u32 subpart;
    u32 reserved[4];
} pfs_inode_t;

int hddReadSectors(u32 lba, u32 nsectors, void *buf);

/* OPL-private helpers for the banked-HDD scanner. These do not change the
   Sony/APA 32-bit API above; they talk only to OPL's xhdd devctl layer. */
static inline int hddReadSectors64(u64 lba, u32 nsectors, void *buf)
{
    hddAtaRead64_t request;

    if (nsectors == 0)
        return -1;

    request.lba_lo = (u32)lba;
    request.lba_hi = (u32)(lba >> 32);
    request.size = nsectors;

    return fileXioDevctl("xhdd0:", ATA_DEVCTL_READ_SECTORS64,
                         &request, sizeof(request), buf, nsectors * 512);
}

static inline int hddGetTotalSectors64(u64 *sectors)
{
    hddLba64_t result;
    int status;

    if (sectors == NULL)
        return -1;

    status = fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_TOTAL_SECTORS64,
                           NULL, 0, &result, sizeof(result));
    if (status == 0)
        *sectors = ((u64)result.hi << 32) | result.lo;

    return status;
}

// Array should be APA_MAXSUB+1 entries.
int hddGetPartitionInfo(const char *name, apa_sub_t *parts);

// Array should be max entries.
int hddGetFileBlockInfo(const char *name, const apa_sub_t *subs, pfs_blockinfo_t *blocks, int max);

#endif
