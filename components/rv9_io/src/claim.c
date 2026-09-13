/*
 * Who owns a device.
 *
 * Everything below the I/O manager is happy to be used twice. Two processes
 * open /gpio/2 and the driver dutifully hands each of them a pin; two
 * processes open /pwm0/3 and the driver allocates each of them an LEDC
 * channel, both of which then drive the same physical output. Nothing
 * fails. The pin simply does whatever the last writer said, which is not a
 * race to be won -- it is two answers to a question with one physical
 * outcome, and the hardware has no way to prefer either.
 *
 * On a machine that only prints, that is a curiosity. On one that moves, it
 * is the difference between an actuator obeying a control loop and an
 * actuator obeying a control loop and also a diagnostic somebody left
 * running. So ownership is decided here, above the drivers and below the
 * programs, in the one place that sees every open.
 *
 * WHAT A RESOURCE IS
 *
 * The full path as opened, not the device: /gpio/2 and /gpio/3 are separate
 * resources on one device, because they are separate pins. Comparison is
 * exact -- whatever text the file manager would resolve, this treats as
 * identity, which means /r0/notes and /r0/NOTES are two resources if the
 * file manager thinks they are two files and one resource claimed twice if
 * it does not. That is a property of the file manager and it is not this
 * layer's business to second-guess.
 *
 * ONE RECORD PER OWNER, NOT PER RESOURCE
 *
 * Five processes sharing /term is five records. A single record with a
 * count would be smaller and would answer "is it busy", which is the
 * question a lock asks. The question an operator asks is "who has it", and
 * the question a failsafe will ask is "whose device was this when it died".
 * Neither survives being reduced to a count.
 *
 * TWO KINDS OF REFERENCE
 *
 * A claim is refcounted, and the references come from two places. Each open
 * path holds one, dropped when the path closes. A manifest reservation
 * holds one, taken at fork before the program runs and dropped when it
 * dies. The second is what makes a declared exclusive device the program's
 * for its lifetime rather than for the length of one open -- and what makes
 * the refusal happen at fork, where it costs nothing, instead of halfway
 * through a control loop's first period.
 *
 * A claim whose owner has exited but whose refcount is not zero stays. That
 * is not a leak: it means an inherited path is still open somewhere, which
 * is exactly the situation where the resource is still in use.
 */
#include <string.h>

#include "esp_log.h"

#include "rv9/io.h"
#include "rv9/kal.h"

static const char *TAG = "rv9-claim";

struct rv9_claim {
    char              name[RV9_CLAIM_NAME_MAX];
    rv9_pid_t         owner;
    bool              exclusive;
    bool              reserved;      /* a fork-time reservation is outstanding */
    uint16_t          refs;

    /*
     * Where this device is to be left when its owner stops.
     *
     * It lives on the ownership record rather than on the process because
     * the two questions are one question: "whose was this" and "what
     * should it be left at" are answered by the same row, and a failsafe
     * for a device nobody owns is not a thing that can exist.
     */
    bool              has_failsafe;
    uint32_t          failsafe_value;

    struct rv9_claim *next;
};

static struct rv9_claim *s_claims;
static rv9_lock_t        s_lock;

rv9_io_err_t rv9_claim_init(void)
{
    if (s_lock != NULL) return RV9_IO_OK;
    return (rv9_lock_create(&s_lock) == RV9_OK) ? RV9_IO_OK : RV9_IO_ERR_NOMEM;
}

void rv9_claim_resource(char *out, size_t cap, const char *dev,
                        const char *rest)
{
    if (out == NULL || cap == 0) return;

    size_t n = 0;
    for (const char *s = dev; s && *s && n + 1 < cap; s++) out[n++] = *s;

    if (rest != NULL && rest[0] != '\0') {
        if (n + 1 < cap) out[n++] = '/';
        for (const char *s = rest; *s && n + 1 < cap; s++) out[n++] = *s;
    }
    out[n] = '\0';
}

/*
 * Take a claim, or say who is in the way.
 *
 * The rule is short enough to state completely: a claim conflicts with
 * another owner's claim on the same resource when either of them is
 * exclusive. Shared against shared never conflicts, which is why every
 * process can still open /term.
 *
 * Asking exclusively for something one already holds shared is an upgrade,
 * and it succeeds precisely when the loop below finds no other owner --
 * which is the same condition as taking it exclusively in the first place.
 */
static rv9_io_err_t take_locked(const char *res, rv9_pid_t owner,
                                bool exclusive, struct rv9_claim **out,
                                rv9_pid_t *blocker)
{
    struct rv9_claim *mine = NULL;

    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (strcmp(c->name, res) != 0) continue;
        if (c->owner == owner) { mine = c; continue; }
        if (c->exclusive || exclusive) {
            if (blocker) *blocker = c->owner;
            return RV9_IO_ERR_BUSY;
        }
    }

    if (mine != NULL) {
        if (exclusive) mine->exclusive = true;
        mine->refs++;
        *out = mine;
        return RV9_IO_OK;
    }

    struct rv9_claim *c = rv9_calloc(1, sizeof(*c));
    if (c == NULL) return RV9_IO_ERR_NOMEM;

    strncpy(c->name, res, sizeof(c->name) - 1);
    c->owner     = owner;
    c->exclusive = exclusive;
    c->refs      = 1;
    c->next      = s_claims;
    s_claims     = c;

    *out = c;
    return RV9_IO_OK;
}

rv9_io_err_t rv9_claim_take(const char *resource, rv9_pid_t owner,
                            bool exclusive, struct rv9_claim **out)
{
    if (resource == NULL || out == NULL) return RV9_IO_ERR_INVAL;
    *out = NULL;

    /* Before rv9_io_init: nothing can conflict, because nothing else has
       opened anything. Refusing here would break boot for no gain. */
    if (s_lock == NULL) return RV9_IO_OK;

    rv9_pid_t blocker = RV9_PID_NONE;

    rv9_lock_acquire(s_lock);
    rv9_io_err_t err = take_locked(resource, owner, exclusive, out, &blocker);
    rv9_lock_release(s_lock);

    if (err == RV9_IO_ERR_BUSY) {
        ESP_LOGW(TAG, "%s belongs to pid %u; pid %u may not open it%s",
                 resource, (unsigned)blocker, (unsigned)owner,
                 exclusive ? " alone" : "");
    }
    return err;
}

/* Caller holds the lock. Returns true if the claim was freed. */
static bool drop_locked(struct rv9_claim *c)
{
    if (c->refs > 0) c->refs--;
    if (c->refs > 0) return false;

    for (struct rv9_claim **pp = &s_claims; *pp; pp = &(*pp)->next) {
        if (*pp == c) { *pp = c->next; break; }
    }
    rv9_free(c);
    return true;
}

void rv9_claim_drop(struct rv9_claim *c)
{
    if (c == NULL || s_lock == NULL) return;

    rv9_lock_acquire(s_lock);
    (void)drop_locked(c);
    rv9_lock_release(s_lock);
}

/*
 * Claim a resource for a process that does not exist yet.
 *
 * Taken from the manifest at fork, so that a program which must own
 * something is refused before it is started rather than after. Repeating
 * the same resource in one manifest is not an error and does not stack: it
 * is one requirement stated twice.
 */
rv9_io_err_t rv9_claim_reserve(const char *resource, rv9_pid_t owner,
                               bool exclusive)
{
    if (resource == NULL) return RV9_IO_ERR_INVAL;
    if (s_lock == NULL) return RV9_IO_OK;

    rv9_lock_acquire(s_lock);

    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (c->owner != owner || strcmp(c->name, resource) != 0) continue;
        if (c->reserved) {
            if (exclusive) c->exclusive = true;
            rv9_lock_release(s_lock);
            return RV9_IO_OK;
        }
    }

    struct rv9_claim *c = NULL;
    rv9_pid_t blocker = RV9_PID_NONE;
    rv9_io_err_t err = take_locked(resource, owner, exclusive, &c, &blocker);
    if (err == RV9_IO_OK) c->reserved = true;

    rv9_lock_release(s_lock);

    if (err == RV9_IO_ERR_BUSY) {
        ESP_LOGE(TAG, "%s belongs to pid %u; pid %u cannot be given it",
                 resource, (unsigned)blocker, (unsigned)owner);
    }
    return err;
}

/*
 * Give back what a process reserved, however it ended.
 *
 * Only the reservations: the paths it opened are closed by the I/O
 * manager's own exit hook, which drops those references separately. Called
 * for an ordinary exit and for a faulted one alike, which is the point --
 * a program that ran off its stack still has to let go of the motor.
 */
void rv9_claim_release_pid(rv9_pid_t owner)
{
    if (s_lock == NULL) return;

    rv9_lock_acquire(s_lock);

    struct rv9_claim *c = s_claims;
    while (c != NULL) {
        struct rv9_claim *next = c->next;
        if (c->owner == owner && c->reserved) {
            c->reserved = false;
            (void)drop_locked(c);
        }
        c = next;
    }

    rv9_lock_release(s_lock);
}

/*
 * Record what a device is to be left at when this owner stops.
 *
 * Refuses unless the caller already owns the resource, which is the
 * admission check rather than an extra one: a program may promise to park
 * what it declared it must own, and nothing else. A failsafe naming
 * somebody else's actuator is a promise it has no standing to make.
 */
rv9_io_err_t rv9_claim_failsafe(const char *resource, rv9_pid_t owner,
                                uint32_t value)
{
    if (resource == NULL) return RV9_IO_ERR_INVAL;
    if (s_lock == NULL)   return RV9_IO_ERR_NOTFOUND;

    rv9_io_err_t err = RV9_IO_ERR_NOTFOUND;

    rv9_lock_acquire(s_lock);
    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (c->owner != owner || strcmp(c->name, resource) != 0) continue;
        c->has_failsafe   = true;
        c->failsafe_value = value;
        err = RV9_IO_OK;
        break;
    }
    rv9_lock_release(s_lock);

    return err;
}

/*
 * Copy out what this owner promised to park, before anything is released.
 *
 * A copy rather than a walk under callback, because applying a failsafe
 * means opening a device -- which takes this lock. The caller gets a
 * snapshot and does the physical work with nothing held.
 */
int rv9_claim_failsafes(rv9_pid_t owner, rv9_claim_fs_t *out, int max)
{
    if (s_lock == NULL) return 0;

    int n = 0;
    rv9_lock_acquire(s_lock);

    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (c->owner != owner || !c->has_failsafe) continue;
        if (out != NULL && n < max) {
            strncpy(out[n].name, c->name, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = '\0';
            out[n].value = c->failsafe_value;
        }
        n++;
    }

    rv9_lock_release(s_lock);
    return n;
}

int rv9_claim_list(rv9_sys_claim_t *out, int max)
{
    if (s_lock == NULL) return 0;

    int n = 0;
    rv9_lock_acquire(s_lock);

    /* Counts every claim, fills what fits. The caller is told how many
       there are, which is not the same as how many it was handed. */
    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (out != NULL && n < max) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, c->name, sizeof(out[n].name) - 1);
            out[n].owner            = c->owner;
            out[n].exclusive        = c->exclusive ? 1 : 0;
            out[n].reserved_at_fork = c->reserved ? 1 : 0;
            out[n].refs             = c->refs;
            out[n].has_failsafe     = c->has_failsafe ? 1 : 0;
            out[n].failsafe_value   = c->failsafe_value;
        }
        n++;
    }

    rv9_lock_release(s_lock);
    return n;
}

bool rv9_claim_owner(const char *resource, rv9_pid_t *out_owner,
                     bool *out_exclusive)
{
    if (resource == NULL || s_lock == NULL) return false;

    bool found = false;
    rv9_lock_acquire(s_lock);

    for (struct rv9_claim *c = s_claims; c; c = c->next) {
        if (strcmp(c->name, resource) != 0) continue;
        found = true;
        if (out_owner)     *out_owner     = c->owner;
        if (out_exclusive) *out_exclusive = c->exclusive;
        if (c->exclusive) break;     /* the definitive answer, if there is one */
    }

    rv9_lock_release(s_lock);
    return found;
}
