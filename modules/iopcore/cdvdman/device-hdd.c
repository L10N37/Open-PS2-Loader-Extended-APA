/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"

#include "device.h"

#include "../../isofs/zso.h"

extern struct cdvdman_settings_hdd cdvdman_settings;

extern struct irx_export_table _exp_atad;

char lba_48bit = 0;
char atad_inited = 0;
static unsigned char CurrentPart = 0;
static unsigned char NumParts;

static hdl_partspecs_t cdvdman_partspecs[HDL_NUM_PART_SPECS];

#ifdef HD_PRO
extern int ata_device_set_write_cache(int device, int enable);
#else
/* OPL's bundled ATAD already carries a u64 LBA internally. Keep the public
   Sony-compatible sceAtaDmaTransfer() interface untouched and call the
   private implementation only from HDD cdvdman. */
extern int ata_device_sector_io_internal(int device, void *buf, u64 lba, u32 nsectors, int dir);
#endif

extern int ata_io_sema;

/* cdvdman_settings.common.padding is unused at runtime and has 0x10 in the
   patch-zone sentinel. Encode the bank as (padding ^ 0x10), which means an
   unmodified/ordinary OPL launch remains Bank 0 without changing the shared
   settings structure. Later the EE launcher can set 0x10 ^ bank_index. */
#define HDD_BANK_PATCH_BIAS 0x10

static unsigned int hddGetBankIndex(void)
{
    return cdvdman_settings.common.padding ^ HDD_BANK_PATCH_BIAS;
}

static u64 hddGetBankBaseSector(void)
{
    return ((u64)hddGetBankIndex()) << 32;
}

static int hddAtaRead(u64 lba, void *buffer, u32 sectors)
{
#ifdef HD_PRO
    /* HD Pro uses a different ATAD implementation. Keep its existing Bank-0
       path intact and explicitly reject higher banks until that driver has a
       separately validated 64-bit raw transfer path. */
    if (lba >> 32)
        return -ENXIO;

    return sceAtaDmaTransfer(0, buffer, (u32)lba, sectors, ATA_DIR_READ);
#else
    return ata_device_sector_io_internal(0, buffer, lba, sectors, ATA_DIR_READ);
#endif
}

static int cdvdman_get_part_specs(u32 lsn)
{
    register int i;
    hdl_partspecs_t *ps;

    for (ps = cdvdman_partspecs, i = 0; i < NumParts; i++, ps++) {
        if ((lsn >= ps->part_offset) && (lsn < (ps->part_offset + (ps->part_size / 2048)))) {
            CurrentPart = i;
            break;
        }
    }

    if (i >= NumParts)
        return -ENXIO;

    return 0;
}

void DeviceInit(void)
{
    RegisterLibraryEntries(&_exp_atad);

    atad_start();
    atad_inited = 1;

    lba_48bit = cdvdman_settings.common.media;

    hdl_apa_header apaHeader;
    int r;
    const unsigned int bankIndex = hddGetBankIndex();
    const u64 bankBaseSector = hddGetBankBaseSector();
    const u64 apaHeaderLba = bankBaseSector + (u64)cdvdman_settings.lba_start;

    DPRINTF("DeviceInit: HDD bank %u, relative APA header LBA = %lu\n", bankIndex, cdvdman_settings.lba_start);

#ifdef HD_PRO
    // For HDPro, as its custom ATAD module does not export sceAtaExecCmd() and sceAtaWaitResult(). And it also resets the ATA bus.
    if (cdvdman_settings.common.flags & IOPCORE_ENABLE_POFF) {
        // If IGR is enabled (the poweroff function here is disabled), we can tell when to flush the cache. Hence if IGR is disabled, then we should disable the write cache.
        ata_device_set_write_cache(0, 0);
    }
#endif

    while ((r = hddAtaRead(apaHeaderLba, &apaHeader, 2)) != 0) {
        DPRINTF("DeviceInit: failed to read apa header %d\n", r);
        DelayThread(2000);
    }

    memcpy(cdvdman_partspecs, apaHeader.part_specs, sizeof(cdvdman_partspecs));

    cdvdman_settings.common.media = apaHeader.discType;
    if (cdvdman_settings.common.layer1_start == 0) // layer1 start not set, read it from APA header
        cdvdman_settings.common.layer1_start = apaHeader.layer1_start;
    NumParts = apaHeader.num_partitions;
}

void DeviceDeinit(void)
{
}

int DeviceReady(void)
{
    return SCECdComplete;
}

void DeviceFSInit(void)
{
}

void DeviceLock(void)
{
    WaitSema(ata_io_sema);
}

void DeviceUnmount(void)
{
    sceAtaFlushCache(0);
}

void DeviceStop(void)
{
    // This will be handled by ATAD.
}

int DeviceReadSectors(u64 lsn, void *buffer, unsigned int sectors)
{
    u32 offset = 0;
    const u64 bankBaseSector = hddGetBankBaseSector();

    while (sectors) {
        if (!(((u32)lsn >= cdvdman_partspecs[CurrentPart].part_offset) && ((u32)lsn < (cdvdman_partspecs[CurrentPart].part_offset + (cdvdman_partspecs[CurrentPart].part_size / 2048)))))
            if (cdvdman_get_part_specs((u32)lsn) != 0)
                return SCECdErTRMOPN;

        u32 nsectors = (cdvdman_partspecs[CurrentPart].part_offset + (cdvdman_partspecs[CurrentPart].part_size / 2048)) - (u32)lsn;
        if (sectors < nsectors)
            nsectors = sectors;

        const u32 relativeLba = cdvdman_partspecs[CurrentPart].data_start + (((u32)lsn - cdvdman_partspecs[CurrentPart].part_offset) << 2);
        const u64 physicalLba = bankBaseSector + (u64)relativeLba;

        if (hddAtaRead(physicalLba, (void *)((u8 *)buffer + offset), nsectors << 2) != 0) {
            return SCECdErREAD;
        }
        offset += nsectors * 2048;
        sectors -= nsectors;
        lsn += nsectors;
    }

    return SCECdErNO;
}
