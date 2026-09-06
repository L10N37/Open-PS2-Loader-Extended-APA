#include "include/opl.h"
#include "include/hdd.h"
#include "include/ioman.h"
#include "include/hddsupport.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

typedef struct // size = 1024
{
    u32 checksum; // HDL uses 0xdeadfeed magic here
    u32 magic;
    char gamename[160];
    u8 hdl_compat_flags;
    u8 ops2l_compat_flags;
    u8 dma_type;
    u8 dma_mode;
    char startup[60];
    u32 layer1_start;
    u32 discType;
    int num_partitions;
    struct
    {
        u32 part_offset; // in MB
        u32 data_start;  // in sectors
        u32 part_size;   // in KB
    } part_specs[65];
} hdl_apa_header;

#define HDL_GAME_DATA_OFFSET    0x100000 // Extended attribute data offset.
#define HDL_GAME_HEADER_OFFSET  ((HDL_GAME_DATA_OFFSET + 4096) / 512) // 0x101000 bytes -> 0x808 sectors.
#define HDL_FS_MAGIC            0x1337
#define HDD_BANK_SECTORS        0x100000000ULL
#define HDD_MAX_BANKS           256
#define HDD_BANK_SCAN_MAX_PARTS 4096

u8 IOBuffer[2048] ALIGNED(64); // one sector

//-------------------------------------------------------------------------
int hddCheck(void)
{
    int ret;

    ret = fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0);
    LOG("HDD: Status is %d\n", ret);
    // 0 = HDD connected and formatted, 1 = not formatted, 2 = HDD not usable, 3 = HDD not connected.
    if ((ret >= 3) || (ret < 0))
        return -1;

    return ret;
}

//-------------------------------------------------------------------------
u32 hddGetTotalSectors(void)
{
    return fileXioDevctl("hdd0:", HDIOC_TOTALSECTOR, NULL, 0, NULL, 0);
}

//-------------------------------------------------------------------------
int hddIs48bit(void)
{
    return fileXioDevctl("xhdd0:", ATA_DEVCTL_IS_48BIT, NULL, 0, NULL, 0);
}

//-------------------------------------------------------------------------
int hddSetTransferMode(int type, int mode)
{
    hddAtaSetMode_t *args = (hddAtaSetMode_t *)IOBuffer;

    args->type = type;
    args->mode = mode;

    return fileXioDevctl("xhdd0:", ATA_DEVCTL_SET_TRANSFER_MODE, args, sizeof(hddAtaSetMode_t), NULL, 0);
}

//-------------------------------------------------------------------------
void hddSetIdleTimeout(int timeout)
{
    // From hdparm man:
    // A value of zero means "timeouts  are  disabled":  the
    // device will not automatically enter standby mode.  Values from 1
    // to 240 specify multiples of 5 seconds, yielding timeouts from  5
    // seconds to 20 minutes.  Values from 241 to 251 specify from 1 to
    // 11 units of 30 minutes, yielding timeouts from 30 minutes to 5.5
    // hours.   A  value  of  252  signifies a timeout of 21 minutes. A
    // value of 253 sets a vendor-defined timeout period between 8  and
    // 12  hours, and the value 254 is reserved.  255 is interpreted as
    // 21 minutes plus 15 seconds.  Note that  some  older  drives  may
    // have very different interpretations of these values.

    u8 standbytimer = (u8)timeout;

    fileXioDevctl("hdd0:", HDIOC_IDLE, &standbytimer, 1, NULL, 0);
    fileXioDevctl("hdd1:", HDIOC_IDLE, &standbytimer, 1, NULL, 0);
}

void hddSetIdleImmediate(void)
{
    fileXioDevctl("hdd0:", HDIOC_IDLEIMM, NULL, 0, NULL, 0);
    fileXioDevctl("hdd1:", HDIOC_IDLEIMM, NULL, 0, NULL, 0);
}

//-------------------------------------------------------------------------
int hddReadSectors(u32 lba, u32 nsectors, void *buf)
{
    hddAtaTransfer_t *args = (hddAtaTransfer_t *)IOBuffer;

    args->lba = lba;
    args->size = nsectors;

    if (fileXioDevctl("hdd0:", HDIOC_READSECTOR, args, sizeof(hddAtaTransfer_t), buf, nsectors * 512) != 0)
        return -1;

    return 0;
}

//-------------------------------------------------------------------------
static int hddWriteSectors(u32 lba, u32 nsectors, const void *buf)
{
    static u8 WriteBuffer[2 * 512 + sizeof(hddAtaTransfer_t)] ALIGNED(64); // Has to be a different buffer from IOBuffer (input can be in IOBuffer).
    int argsz;
    hddAtaTransfer_t *args = (hddAtaTransfer_t *)WriteBuffer;

    if (nsectors > 2) // Sanity check
        return -ENOMEM;

    args->lba = lba;
    args->size = nsectors;
    memcpy(args->data, buf, nsectors * 512);

    argsz = sizeof(hddAtaTransfer_t) + (nsectors * 512);

    if (fileXioDevctl("hdd0:", HDIOC_WRITESECTOR, args, argsz, NULL, 0) != 0)
        return -1;

    return 0;
}

//-------------------------------------------------------------------------
struct GameDataEntry
{
    u32 lba;        // Bank-relative HDL metadata sector (main partition + 0x808).
    u32 size;       // Allocated size in 2048-byte sectors, matching legacy Bank-0 scan.
    u32 bank_index;
    struct GameDataEntry *next;
    char id[APA_IDMAX + 1];
};

static u64 hddBankBase(u32 bank_index)
{
    return ((u64)bank_index) << 32;
}

static int hddReadGameSectors(const struct GameDataEntry *game, u32 nsectors, void *buf)
{
    if (game->bank_index == 0)
        return hddReadSectors(game->lba, nsectors, buf);

    return hddReadSectors64(hddBankBase(game->bank_index) + game->lba, nsectors, buf);
}

static int hddGetHDLGameInfo(struct GameDataEntry *game, hdl_game_info_t *ginfo)
{
    int ret;

    ret = hddReadGameSectors(game, 2, IOBuffer);
    if (ret == 0) {
        hdl_apa_header *hdl_header = (hdl_apa_header *)IOBuffer;

        strncpy(ginfo->partition_name, game->id, APA_IDMAX);
        ginfo->partition_name[APA_IDMAX] = '\0';
        strncpy(ginfo->name, hdl_header->gamename, HDL_GAME_NAME_MAX);
        ginfo->name[HDL_GAME_NAME_MAX] = '\0';
        strncpy(ginfo->startup, hdl_header->startup, sizeof(ginfo->startup) - 1);
        ginfo->startup[sizeof(ginfo->startup) - 1] = '\0';
        ginfo->hdl_compat_flags = hdl_header->hdl_compat_flags;
        ginfo->ops2l_compat_flags = hdl_header->ops2l_compat_flags;
        ginfo->dma_type = hdl_header->dma_type;
        ginfo->dma_mode = hdl_header->dma_mode;
        ginfo->layer_break = hdl_header->layer1_start;
        ginfo->disctype = (u8)hdl_header->discType;
        ginfo->start_sector = game->lba;
        ginfo->total_size_in_kb = game->size * 2; // size * 2048 / 1024 = 2x
        ginfo->bank_index = game->bank_index;
    } else
        ret = -1;

    return ret;
}

//-------------------------------------------------------------------------
static struct GameDataEntry *GetGameListRecord(struct GameDataEntry *head, const char *partition, u32 bank_index)
{
    struct GameDataEntry *current;

    for (current = head; current != NULL; current = current->next) {
        if (current->bank_index == bank_index && !strncmp(current->id, partition, APA_IDMAX))
            return current;
    }

    return NULL;
}

static struct GameDataEntry *AppendGameListRecord(struct GameDataEntry **head, struct GameDataEntry **tail, const char *partition, u32 bank_index)
{
    struct GameDataEntry *entry = malloc(sizeof(struct GameDataEntry));

    if (entry == NULL)
        return NULL;

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->id, partition, APA_IDMAX);
    entry->id[APA_IDMAX] = '\0';
    entry->bank_index = bank_index;

    if (*tail != NULL)
        (*tail)->next = entry;
    else
        *head = entry;

    *tail = entry;
    return entry;
}

static int hddApaHeaderChecksumValid(const apa_header_t *header)
{
    const u32 *words = (const u32 *)header;
    u32 checksum = 0;
    int i;

    if (header->magic != APA_MAGIC)
        return 0;

    // APA checksum is the sum of the remaining 255 words with word 0 omitted.
    for (i = 1; i < (int)(sizeof(apa_header_t) / sizeof(u32)); i++)
        checksum += words[i];

    return checksum == header->checksum;
}

static int hddReadUpperBankHeader(u32 bank_index, u32 relative_lba, apa_header_t *header)
{
    return hddReadSectors64(hddBankBase(bank_index) + relative_lba,
                            sizeof(apa_header_t) / 512, header);
}

/* Scan one independent APA bank without mounting it through Sony hdd.irx.
   Bank 1+ remains read-only at this stage. */
static int hddScanUpperBank(u32 bank_index, u64 total_sectors, struct GameDataEntry **head, struct GameDataEntry **tail, u32 *count)
{
    apa_header_t *header = (apa_header_t *)IOBuffer;
    const u64 bank_base = hddBankBase(bank_index);
    const u64 remaining = total_sectors - bank_base;
    const u64 bank_sectors = remaining < HDD_BANK_SECTORS ? remaining : HDD_BANK_SECTORS;
    u32 current_lba = 0;
    u32 previous_lba = 0;
    unsigned int visited = 0;

    if (bank_sectors < 2)
        return 0;

    if (hddReadUpperBankHeader(bank_index, 0, header) != 0)
        return -EIO;

    // A bank is considered present only when sector 0 is a valid ordinary APA MBR.
    if (!hddApaHeaderChecksumValid(header) || header->start != 0 || strncmp(header->id, "__mbr", 5) != 0)
        return 0;

    do {
        u32 next_lba;

        if (visited++ >= HDD_BANK_SCAN_MAX_PARTS) {
            LOG("HDD bank %u: APA chain exceeded safety limit.\n", bank_index);
            return -EIO;
        }

        if (current_lba != 0) {
            if ((u64)current_lba >= bank_sectors) {
                LOG("HDD bank %u: APA LBA 0x%08X outside bank.\n", bank_index, current_lba);
                return -EIO;
            }

            if (hddReadUpperBankHeader(bank_index, current_lba, header) != 0)
                return -EIO;

            if (!hddApaHeaderChecksumValid(header) || header->start != current_lba) {
                LOG("HDD bank %u: invalid APA header at 0x%08X.\n", bank_index, current_lba);
                return -EIO;
            }

            if (header->prev != previous_lba) {
                LOG("HDD bank %u: broken APA prev link at 0x%08X.\n", bank_index, current_lba);
                return -EIO;
            }
        }

        if (header->type == HDL_FS_MAGIC && !(header->flags & APA_FLAG_SUB)) {
            struct GameDataEntry *entry;
            u64 allocated_sectors = header->length;
            u32 i;

            if (header->nsub > APA_MAXSUB) {
                LOG("HDD bank %u: invalid HDL nsub %u at 0x%08X.\n", bank_index, header->nsub, current_lba);
                return -EIO;
            }

            for (i = 0; i < header->nsub; i++) {
                if ((u64)header->subs[i].start >= bank_sectors ||
                    (u64)header->subs[i].start + header->subs[i].length > bank_sectors) {
                    LOG("HDD bank %u: HDL subpartition outside bank.\n", bank_index);
                    return -EIO;
                }
                allocated_sectors += header->subs[i].length;
            }

            if ((u64)header->start + header->length > bank_sectors ||
                (u64)header->start + HDL_GAME_HEADER_OFFSET + 2 > bank_sectors) {
                LOG("HDD bank %u: HDL main partition outside bank.\n", bank_index);
                return -EIO;
            }

            entry = AppendGameListRecord(head, tail, header->id, bank_index);
            if (entry == NULL)
                return -ENOMEM;

            entry->lba = header->start + HDL_GAME_HEADER_OFFSET;
            entry->size = (u32)(allocated_sectors / 4); // 512-byte sectors -> 2048-byte sectors.
            (*count)++;
        }

        next_lba = header->next;
        if (next_lba == 0)
            break;

        if (next_lba == current_lba || (u64)next_lba >= bank_sectors) {
            LOG("HDD bank %u: invalid APA next link 0x%08X.\n", bank_index, next_lba);
            return -EIO;
        }

        previous_lba = current_lba;
        current_lba = next_lba;
    } while (current_lba != 0);

    return 0;
}

static int hddScanAdditionalBanks(struct GameDataEntry **head, struct GameDataEntry **tail, u32 *count)
{
    u64 total_sectors;
    u64 last_bank;
    u32 bank_index;
    int result;

    result = hddGetTotalSectors64(&total_sectors);
    if (result != 0) {
        LOG("HDD: 64-bit capacity query unavailable (%d); Bank 0 only.\n", result);
        return 0;
    }

    if (total_sectors <= HDD_BANK_SECTORS)
        return 0;

    last_bank = (total_sectors - 1) >> 32;
    if (last_bank >= HDD_MAX_BANKS)
        last_bank = HDD_MAX_BANKS - 1;

    LOG("HDD: 64-bit capacity detected; scanning %llu additional bank(s).\n", last_bank);

    for (bank_index = 1; bank_index <= (u32)last_bank; bank_index++) {
        result = hddScanUpperBank(bank_index, total_sectors, head, tail, count);
        if (result != 0) {
            // One damaged/unsupported upper bank must not hide valid games from Bank 0
            // or later banks. Log it and continue with the next physical bank.
            LOG("HDD: Bank %u scan failed (%d); skipping bank.\n", bank_index, result);
        }
    }

    return 0;
}

int hddGetHDLGamelist(hdl_games_list_t *game_list)
{
    struct GameDataEntry *head, *tail, *current, *next, *pGameEntry;
    unsigned int count, i;
    iox_dirent_t dirent;
    int fd, ret;

    hddFreeHDLGamelist(game_list);

    head = tail = NULL;
    count = 0;
    ret = 0;

    // Bank 0 remains entirely on the existing Sony hdd.irx directory path.
    if ((fd = fileXioDopen("hdd0:")) >= 0) {
        while (fileXioDread(fd, &dirent) > 0) {
            if (dirent.stat.mode == HDL_FS_MAGIC) {
                if ((pGameEntry = GetGameListRecord(head, dirent.name, 0)) == NULL) {
                    pGameEntry = AppendGameListRecord(&head, &tail, dirent.name, 0);
                    if (pGameEntry == NULL) {
                        ret = -ENOMEM;
                        break;
                    }
                    count++;
                }

                if (!(dirent.stat.attr & APA_FLAG_SUB)) {
                    // Note: The APA specification states that there is a 4KB area used for storing the partition's information, before the extended attribute area.
                    pGameEntry->lba = dirent.stat.private_5 + HDL_GAME_HEADER_OFFSET;
                }

                pGameEntry->size += (dirent.stat.size / 4); // size in HDD sectors * (512 / 2048) = 0.25x
            }
        }

        fileXioDclose(fd);
    } else {
        ret = fd;
    }

    if (ret == 0)
        hddScanAdditionalBanks(&head, &tail, &count);

    if (ret == 0 && head != NULL) {
        if ((game_list->games = malloc(sizeof(hdl_game_info_t) * count)) != NULL) {
            memset(game_list->games, 0, sizeof(hdl_game_info_t) * count);

            for (i = 0, current = head; i < count && current != NULL; i++, current = current->next) {
                if ((ret = hddGetHDLGameInfo(current, &game_list->games[i])) != 0)
                    break;
            }

            if (ret) {
                free(game_list->games);
                game_list->games = NULL;
            } else {
                game_list->count = count;
            }
        } else {
            ret = -ENOMEM;
        }
    }

    for (current = head; current != NULL; current = next) {
        next = current->next;
        free(current);
    }

    return ret;
}

//-------------------------------------------------------------------------
void hddFreeHDLGamelist(hdl_games_list_t *game_list)
{
    if (game_list->games != NULL) {
        free(game_list->games);
        game_list->games = NULL;
        game_list->count = 0;
    }
}

//-------------------------------------------------------------------------
int hddSetHDLGameInfo(hdl_game_info_t *ginfo)
{
    // Upper banks are intentionally read-only during initial validation.
    if (ginfo->bank_index != 0)
        return -ENOSYS;

    if (hddReadSectors(ginfo->start_sector, 2, IOBuffer) != 0)
        return -EIO;

    hdl_apa_header *hdl_header = (hdl_apa_header *)IOBuffer;

    // just change game name and compat flags !!!
    strncpy(hdl_header->gamename, ginfo->name, sizeof(hdl_header->gamename));
    hdl_header->gamename[sizeof(hdl_header->gamename) - 1] = '\0';
    // hdl_header->hdl_compat_flags = ginfo->hdl_compat_flags;
    hdl_header->ops2l_compat_flags = ginfo->ops2l_compat_flags;
    hdl_header->dma_type = ginfo->dma_type;
    hdl_header->dma_mode = ginfo->dma_mode;

    if (hddWriteSectors(ginfo->start_sector, 2, IOBuffer) != 0)
        return -EIO;

    return 0;
}

//-------------------------------------------------------------------------
int hddDeleteHDLGame(hdl_game_info_t *ginfo)
{
    char path[38];

    // Upper banks are intentionally read-only during initial validation.
    if (ginfo->bank_index != 0)
        return -ENOSYS;

    LOG("HDD Delete game: '%s'\n", ginfo->name);

    sprintf(path, "hdd0:%s", ginfo->partition_name);

    return unlink(path);
}

//-------------------------------------------------------------------------
int hddGetPartitionInfo(const char *name, apa_sub_t *parts)
{
    u32 lba;
    iox_stat_t stat;
    apa_header_t *header;
    int result, i;

    if ((result = fileXioGetStat(name, &stat)) >= 0) {
        lba = stat.private_5;
        header = (apa_header_t *)IOBuffer;

        if (hddReadSectors(lba, sizeof(apa_header_t) / 512, header) == 0) {
            parts[0].start = header->start;
            parts[0].length = header->length;

            for (i = 0; i < header->nsub; i++)
                parts[1 + i] = header->subs[i];

            result = header->nsub + 1;
        } else
            result = -EIO;
    }

    return result;
}

//-------------------------------------------------------------------------
int hddGetFileBlockInfo(const char *name, const apa_sub_t *subs, pfs_blockinfo_t *blocks, int max)
{
    u32 lba;
    iox_stat_t stat;
    pfs_inode_t *inode;
    int result;

    if ((result = fileXioGetStat(name, &stat)) >= 0) {
        lba = subs[stat.private_4].start + stat.private_5;
        inode = (pfs_inode_t *)IOBuffer;

        if (hddReadSectors(lba, sizeof(pfs_inode_t) / 512, inode) == 0) {
            if (inode->number_data < max) {
                memcpy(blocks, inode->data, max * sizeof(pfs_blockinfo_t));
                result = inode->number_data;
            } else
                result = -ENOMEM;
        } else
            result = -EIO;
    }

    return result;
}
