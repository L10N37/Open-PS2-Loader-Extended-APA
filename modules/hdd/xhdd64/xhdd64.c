#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <atad.h>
#include <iomanX.h>
#include <errno.h>

#include "opl-hdd-ioctl.h"
#include "xhdd.h"
#include "../xhdd/ata_identify.h"

#define MODNAME "xhdd64"
IRX_ID(MODNAME, 1, 0);

static IDENTIFY_DEVICE_DATA deviceIdentifyData;

static int xhdd64Init(iop_device_t *device)
{
    // Force ATAD to initialize the HDD device table.
    sceAtaInit(0);
    return 0;
}

static int xhdd64Unsupported(void)
{
    return -1;
}

static int xhdd64Devctl(iop_file_t *fd, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
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

            return ata_device_set_transfer_mode(fd->unit, ((hddAtaSetMode_t *)arg)->type, ((hddAtaSetMode_t *)arg)->mode);
        }

        case ATA_DEVCTL_READ_PARTITION_SECTOR: {
            if (buf == NULL || buflen == 0 || (buflen % 512) != 0)
                return -EINVAL;

            return sceAtaDmaTransfer(fd->unit, buf, 0, buflen / 512, ATA_DIR_READ);
        }

        case ATA_DEVCTL_GET_HIGHEST_UDMA_MODE: {
            int result = ata_device_identify(fd->unit, &deviceIdentifyData);
            if (result != 0)
                return result;

            for (int i = 7; i >= 0; i--) {
                if ((deviceIdentifyData.UltraDMASupport & (1 << i)) != 0)
                    return i;
            }

            return -EINVAL;
        }

        case ATA_DEVCTL_READ_SECTORS64: {
            hddAtaRead64_t *request;
            u64 lba;
            u32 sectors;

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

            return ata_device_sector_io64(fd->unit, buf, lba, sectors, ATA_DIR_READ);
        }

        default:
            return -EINVAL;
    }
}

static iop_device_ops_t xhdd64_ops = {
    &xhdd64Init,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    (void *)&xhdd64Unsupported,
    &xhdd64Devctl,
};

static iop_device_t xhdd64Device = {
    "xhdd",
    IOP_DT_BLOCK | IOP_DT_FSEXT,
    1,
    "XHDD64",
    &xhdd64_ops};

int _start(int argc, char *argv[])
{
    return AddDrv(&xhdd64Device) == 0 ? MODULE_RESIDENT_END : MODULE_NO_RESIDENT_END;
}
