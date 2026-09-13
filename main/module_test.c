/*
 * The module manifest, tested against images built here in memory.
 *
 * A manifest is read off flash, which means it is data written by
 * somebody else and must be treated as hostile: a length that runs past
 * the end of the module, an offset pointing into the header, an entry that
 * does not advance. Those cases cannot be produced by mkmodule.py, so a
 * test that only reads well-formed modules tests nothing that matters.
 *
 * Building the images here rather than flashing them also means the
 * refusal path -- an unknown mandatory tag -- can be checked without
 * needing a way to get a deliberately broken module onto the board.
 */
#include "module_test.h"

#include "rv9/module.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "mod-test";

static int s_passed;
static int s_failed;

static void check(bool ok, const char *what)
{
    if (ok) { s_passed++; ESP_LOGI(TAG, "  pass  %s", what); }
    else    { s_failed++; ESP_LOGE(TAG, "  FAIL  %s", what); }
}

/* ---- building an image ---- */

#define IMG_MAX 256

typedef struct {
    uint8_t  buf[IMG_MAX];
    uint32_t len;
    uint32_t manifest_off;
} img_t;

static void img_begin(img_t *m)
{
    memset(m, 0, sizeof(*m));

    /* Header, then a padded name, then the manifest. Same layout as
       tools/mkmodule.py, which is the point: if the two disagree the test
       is testing the wrong thing. */
    m->len = RV9_MODULE_HDR_LEN;
    memcpy(m->buf + m->len, "test\0\0\0\0", 8);
    m->len += 8;
    m->manifest_off = m->len;
}

static void img_tag(img_t *m, uint16_t tag, const void *value, uint16_t len)
{
    rv9_mod_tlv_t e = { .tag = tag, .len = len };
    memcpy(m->buf + m->len, &e, sizeof(e));
    m->len += sizeof(e);

    if (len) memcpy(m->buf + m->len, value, len);
    m->len += len;
    m->len += (4 - (m->len % 4)) % 4;
}

static void img_u32(img_t *m, uint16_t tag, uint32_t v)
{
    img_tag(m, tag, &v, sizeof(v));
}

/* Close the list, fill the header and compute the CRC the way the loader
   will check it. */
static void img_end(img_t *m, bool with_manifest)
{
    img_tag(m, RV9_MTAG_END, NULL, 0);

    uint32_t entry_off = m->len;
    m->buf[m->len++] = 0x67;        /* one byte of "code" */
    m->len += (4 - (m->len % 4)) % 4;

    rv9_mod_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic           = RV9_MODULE_MAGIC;
    h.header_len      = RV9_MODULE_HDR_LEN;
    h.abi_version     = 1;
    h.module_len      = m->len;
    h.name_offset     = RV9_MODULE_HDR_LEN;
    h.entry_offset    = entry_off;
    h.type            = RV9_MOD_PROGRAM;
    h.revision        = 1;
    h.manifest_offset = with_manifest ? m->manifest_off : 0;
    memcpy(m->buf, &h, sizeof(h));

    uint32_t crc = rv9_crc32(0, m->buf, m->len);
    memcpy(m->buf + offsetof(rv9_mod_header_t, crc32), &crc, sizeof(crc));
}

/* ---- the tests ---- */

bool rv9_module_selftest(void)
{
    ESP_LOGI(TAG, "module manifest");

    s_passed = 0;
    s_failed = 0;

    /* --- a well-formed manifest --- */
    {
        img_t m;
        img_begin(&m);
        img_tag(&m, RV9_MTAG_DESC, "a test", 6);
        img_u32(&m, RV9_MTAG_STACK, 1536);
        img_u32(&m, RV9_MTAG_HEAP_MAX | RV9_MTAG_MANDATORY, 0);
        img_tag(&m, RV9_MTAG_DEVICE, "/pwm0", 5);
        img_tag(&m, RV9_MTAG_DEVICE, "/adc0", 5);
        img_end(&m, true);

        check(rv9_mod_verify(m.buf, m.len) == RV9_MOD_OK,
              "a module carrying a manifest verifies");

        uint32_t v = 0;
        check(rv9_mod_manifest_u32(m.buf, RV9_MTAG_STACK, &v) && v == 1536,
              "a number comes back");

        v = 0xFFFF;
        check(rv9_mod_manifest_u32(m.buf, RV9_MTAG_HEAP_MAX, &v) && v == 0,
              "so does a mandatory one, found by its plain tag");

        uint16_t len = 0;
        const void *d = rv9_mod_manifest_find(m.buf, RV9_MTAG_DESC, NULL, &len);
        check(d != NULL && len == 6 && memcmp(d, "a test", 6) == 0,
              "and a string, with its length");

        /* Repeats are walked by handing the previous one back. */
        const void *first = rv9_mod_manifest_find(m.buf, RV9_MTAG_DEVICE,
                                                  NULL, &len);
        const void *second = rv9_mod_manifest_find(m.buf, RV9_MTAG_DEVICE,
                                                   first, &len);
        const void *third = rv9_mod_manifest_find(m.buf, RV9_MTAG_DEVICE,
                                                  second, &len);
        check(first && second && first != second && third == NULL,
              "a repeated tag is walked and then runs out");
        check(second && len == 5 && memcmp(second, "/adc0", 5) == 0,
              "in the order it was written");

        check(rv9_mod_manifest_find(m.buf, RV9_MTAG_FAILSAFE, NULL, NULL)
                  == NULL,
              "an absent tag is absent, not an error");
        check(!rv9_mod_manifest_u32(m.buf, RV9_MTAG_DESC, &v),
              "and a string is not read as a number");
    }

    /* --- an unknown tag --- */
    {
        img_t m;
        img_begin(&m);
        img_u32(&m, 0x0400, 12345);         /* advisory, and unknown */
        img_u32(&m, RV9_MTAG_STACK, 2048);
        img_end(&m, true);

        check(rv9_mod_verify(m.buf, m.len) == RV9_MOD_OK,
              "an unknown advisory tag is skipped");

        uint32_t v = 0;
        check(rv9_mod_manifest_u32(m.buf, RV9_MTAG_STACK, &v) && v == 2048,
              "and does not derail the walk");
    }

    /* --- an unknown tag that insists --- */
    {
        img_t m;
        img_begin(&m);
        img_u32(&m, 0x0400 | RV9_MTAG_MANDATORY, 1);
        img_end(&m, true);

        check(rv9_mod_verify(m.buf, m.len) == RV9_MOD_ERR_CONTRACT,
              "an unknown MANDATORY tag is refused");
    }

    /* --- no manifest at all, which is every module built before this --- */
    {
        img_t m;
        img_begin(&m);
        img_end(&m, false);

        check(rv9_mod_verify(m.buf, m.len) == RV9_MOD_OK,
              "a module with no manifest still loads");
        check(rv9_mod_manifest_find(m.buf, RV9_MTAG_STACK, NULL, NULL) == NULL,
              "and answers nothing to everything");
    }

    /* --- malformed, which is the case that matters --- */
    {
        img_t m;
        img_begin(&m);
        img_u32(&m, RV9_MTAG_STACK, 1024);
        img_end(&m, true);

        /* A length that reaches past the end of the module. The CRC is
           recomputed so the walk is reached at all. */
        uint16_t huge = 4000;
        memcpy(m.buf + m.manifest_off + 2, &huge, sizeof(huge));
        uint32_t zero = 0;
        memcpy(m.buf + offsetof(rv9_mod_header_t, crc32), &zero, sizeof(zero));
        uint32_t crc = rv9_crc32(0, m.buf, m.len);
        memcpy(m.buf + offsetof(rv9_mod_header_t, crc32), &crc, sizeof(crc));

        check(rv9_mod_verify(m.buf, m.len) == RV9_MOD_OK,
              "a value running off the end stops the walk, quietly");
        check(rv9_mod_manifest_find(m.buf, RV9_MTAG_STACK, NULL, NULL) == NULL,
              "and yields nothing rather than the memory after it");
    }

    {
        img_t m;
        img_begin(&m);
        img_u32(&m, RV9_MTAG_STACK, 1024);
        img_end(&m, true);

        /* An offset pointing inside the header, and one past the end. */
        uint32_t bad = 8;
        memcpy(m.buf + offsetof(rv9_mod_header_t, manifest_offset),
               &bad, sizeof(bad));
        check(rv9_mod_manifest_find(m.buf, RV9_MTAG_STACK, NULL, NULL) == NULL,
              "a manifest inside the header is ignored");

        bad = m.len + 64;
        memcpy(m.buf + offsetof(rv9_mod_header_t, manifest_offset),
               &bad, sizeof(bad));
        check(rv9_mod_manifest_find(m.buf, RV9_MTAG_STACK, NULL, NULL) == NULL,
              "and one past the end of the module");
    }

    if (s_failed == 0) ESP_LOGI(TAG, "%d passed, 0 failed", s_passed);
    else               ESP_LOGE(TAG, "%d passed, %d FAILED", s_passed, s_failed);

    return s_failed == 0;
}
