/*
 * df -- how much room a volume has.  df /sd0
 *
 * Sizes in kilobytes, because a 32 GB card in 512-byte sectors is sixty-two
 * million of them and nobody reads that. The count is taken by reading the
 * volume's bitmap when asked; on a large card it takes a moment.
 */
#include "modlib.h"

/* The file manager's own question; see components/rv9_io/include/rv9/io.h. */
#define RV9_RBF_GS_SPACE (RV9_SS_DRIVER_BASE + 65)

typedef struct __attribute__((packed)) {
    uint32_t sector_size;
    uint32_t total_sectors;
    uint32_t free_sectors;
} rbf_space_t;

typedef struct { rbf_space_t space; } df_statics_t;

static void kbytes(const rv9_mod_env_t *env, uint32_t sectors, uint32_t ssize)
{
    /* sectors * 512 / 1024, without overflowing 32 bits on a big card. */
    uint32_t kb = (ssize >= 1024) ? sectors * (ssize / 1024)
                                  : sectors / (1024 / ssize);
    m_num(env, RV9_STDOUT, (int32_t)kb);
    m_say(env, RV9_STDOUT, " KB");
}

__attribute__((section(".text.entry")))
int rv9_module_entry(const rv9_mod_env_t *env)
{
    if (env == NULL || env->abi_version < 9) return -1;

    df_statics_t *st = (df_statics_t *)env->statics;
    if (st == NULL || env->statics_size < sizeof(*st)) return -2;

    const char *dev = (env->arg && env->arg[0]) ? env->arg : "/r0";

    int p = env->open(dev, RV9_MODE_READ);
    if (p < 0) {
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, ": cannot open\n");
        return -3;
    }

    int rc = env->getstat(p, RV9_RBF_GS_SPACE, &st->space);
    env->close(p);

    if (rc < 0) {
        m_say(env, RV9_STDOUT, dev);
        m_say(env, RV9_STDOUT, ": not a volume that counts its room\n");
        return -4;
    }

    uint32_t used = st->space.total_sectors - st->space.free_sectors;

    m_say(env, RV9_STDOUT, dev);
    m_say(env, RV9_STDOUT, "  size ");
    kbytes(env, st->space.total_sectors, st->space.sector_size);
    m_say(env, RV9_STDOUT, "  used ");
    kbytes(env, used, st->space.sector_size);
    m_say(env, RV9_STDOUT, "  free ");
    kbytes(env, st->space.free_sectors, st->space.sector_size);
    m_say(env, RV9_STDOUT, "\n");
    return 0;
}
