#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <atad.h>
#include <iomanX.h>
#include <errno.h>

#include "opl-hdd-ioctl.h"
#include "xhdd.h"
#include "ata_identify.h"

#define MODNAME "xhdd"
IRX_ID(MODNAME, 1, 3);

static int isHDPro;
static IDENTIFY_DEVICE_DATA deviceIdentifyData;

typedef int (*ata_sector_io64_fn)(int device, void *buf, u64 lba, u32 nsectors, int dir);
static ata_sector_io64_fn ataSectorIo64 = NULL;

static void xhddResolveAta64(void)
{
    iop_library_t *library;
    lc_internals_t *loadcore;

    ataSectorIo64 = NULL;

    // HD Pro intentionally does not implement ATAD export 19. Keep that
    // configuration on the original 32-bit/Bank-0 path.
    if (isHDPro)
        return;

    loadcore = GetLoadcoreInternalData();
    if (loadcore == NULL)
        return;

    for (library = loadcore->let_next; library != NULL; library = library->prev) {
        if (!strcmp(library->name, "atad")) {
            // Current PS2SDK ATAD 1.3 exports ata_device_sector_io64 at ordinal 19.
            if (library->version >= 0x0103)
                ataSectorIo64 = (ata_sector_io64_fn)library->exports[19];
            break;
        }
    }
}

static int xhddInit(iop_device_t *device)
{
    // Force atad to initialize the hdd devices.
    sceAtaInit(0);
    xhddResolveAta64();

    return 0;
}

static int xhddUnsupported(void)
{
    return -1;
}

static int xhddDevctl(iop_file_t *fd, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
    ata_devinfo_t *devinfo;

    if (fd->unit >= 2)
        return -ENXIO;

    switch (cmd) {
        case ATA_DEVCTL_IS_48BIT:
            return ((devinfo = sceAtaInit(fd->unit)) != NULL ? devinfo->lba48 : -1);

        case ATA_DEVCTL_SET_TRANSFER_MODE: {
            if (arg == NULL || arglen < sizeof(hddAtaSetMode_t))
                return -EINVAL;

            if (!isHDPro)
                return ata_device_set_transfer_mode(fd->unit, ((hddAtaSetMode_t *)arg)->type, ((hddAtaSetMode_t *)arg)->mode);
            else
                return hdproata_device_set_transfer_mode(fd->unit, ((hddAtaSetMode_t *)arg)->type, ((hddAtaSetMode_t *)arg)->mode);
        }

        case ATA_DEVCTL_READ_PARTITION_SECTOR: {
            // Make sure the length is a multiple of the device sector size.
            if (buf == NULL || buflen == 0 || (buflen % 512) != 0)
                return -EINVAL;

            return sceAtaDmaTransfer(fd->unit, buf, 0, buflen / 512, ATA_DIR_READ);
        }

        case ATA_DEVCTL_GET_HIGHEST_UDMA_MODE: {
            // Get the device info.
            int result = ata_device_identify(fd->unit, &deviceIdentifyData);
            if (result != 0)
                return result;

            // Check the highest UDMA mode supported.
            for (int i = 7; i >= 0; i--) {
                // Check if the current UDMA mode is supported.
                if ((deviceIdentifyData.UltraDMASupport & (1 << i)) != 0)
                    return i;
            }

            return -EINVAL;
        }

        case ATA_DEVCTL_READ_SECTORS64: {
            hddAtaRead64_t *request;
            u64 lba;
            u32 sectors;

            if (ataSectorIo64 == NULL)
                return -ENOSYS;
            if (arg == NULL || arglen < sizeof(hddAtaRead64_t) || buf == NULL)
                return -EINVAL;

            request = (hddAtaRead64_t *)arg;
            sectors = request->size;
            if (sectors == 0 || sectors > (buflen / 512) || buflen < sectors * 512)
                return -EINVAL;

            devinfo = sceAtaInit(fd->unit);
            if (devinfo == NULL)
                return -ENXIO;

            lba = ((u64)request->lba_hi << 32) | request->lba_lo;

            // Any bank above Bank 0 necessarily requires LBA48.
            if ((request->lba_hi != 0) && !devinfo->lba48)
                return -ENXIO;

            return ataSectorIo64(fd->unit, buf, lba, sectors, ATA_DIR_READ);
        }

        case ATA_DEVCTL_GET_TOTAL_SECTORS64: {
            hddLba64_t *result;
            int identifyResult;

            if (buf == NULL || buflen < sizeof(hddLba64_t))
                return -EINVAL;

            identifyResult = ata_device_identify(fd->unit, &deviceIdentifyData);
            if (identifyResult != 0)
                return identifyResult;

            result = (hddLba64_t *)buf;

            if (deviceIdentifyData.CommandSetSupport.BigLba) {
                // IDENTIFY words 100-103: total number of 48-bit-LBA user sectors.
                result->lo = deviceIdentifyData.Max48BitLBA[0];
                result->hi = deviceIdentifyData.Max48BitLBA[1];
            } else {
                result->lo = deviceIdentifyData.UserAddressableSectors;
                result->hi = 0;
            }

            return 0;
        }

        default:
            return -EINVAL;
    }
}

static iop_device_ops_t xhdd_ops = {
    &xhddInit,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    &xhddDevctl,
};

static iop_device_t xhddDevice = {
    "xhdd",
    IOP_DT_BLOCK | IOP_DT_FSEXT,
    1,
    "XHDD",
    &xhdd_ops};

int _start(int argc, char *argv[])
{
    int i;

    isHDPro = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-hdpro"))
            isHDPro = 1;
    }

    return AddDrv(&xhddDevice) == 0 ? MODULE_RESIDENT_END : MODULE_NO_RESIDENT_END;
}
