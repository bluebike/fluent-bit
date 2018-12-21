/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_parser.h>
#include <fluent-bit/flb_error.h>

#include <time.h>
#include "flb_tests_internal.h"

/* Parsers configuration */
#define JSON_PARSERS  FLB_TESTS_DATA_PATH "/data/parser/json.conf"
#define REGEX_PARSERS FLB_TESTS_DATA_PATH "/data/parser/regex.conf"

/* Templates */
#define JSON_FMT_01  "{\"key001\": 12345, \"key002\": 0.99, \"time\": \"%s\"}"
#define REGEX_FMT_01 "12345 0.99 %s"

/* Timezone */
struct tz_check {
    char *val;
    int diff;
};

struct tz_check tz_entries_ok[] = {
    {"+0000",       0},
    {"+00:00",      0},
    {"+00:59",   3540},
    {"-0600",  -21000},
    {"-06:00", -21000},
};

struct tz_check tz_entries_error[] = {
    {"0000",   0},
    {"+00:90", 0},
    {"--600",  0},
    {"-06:00", -21000},
};

/* Time Lookup */
struct time_check {
    char *parser_name;
    char *time_string;
    time_t epoch;
    double frac_seconds;

    /*
     * Some tests requires to set the UTC offset to the given time,
     * when this flag is enabled the parser is adjusted for each test.
     */
    int utc_offset;
};

struct time_check time_entries[] = {
    /*
     * Samples generated by scripts/dates.sh
     * =====================================
     * UTC => 07/17/2017 20:17:03 +0000, 1500322623
     * IST => 07/18/2017 01:47:03 +0530, 1500322623
     * JP  => 07/18/2017 05:17:03 +0900, 1500322623
     * ZW  => 07/17/2017 22:17:03 +0200, 1500322623
     */

    /*
     * No year tests (old Syslog)
     * ==========================
     */

    /* Fixed UTC Offset = -0600 (-21600) */
    {"no_year"     , "Feb 16 04:06:58"           , 1487239618, 0     , -21600},
    {"no_year_N"   , "Feb 16 04:06:58.1234"      , 1487239618, 0.1234, -21600},
    {"no_year_NC"  , "Feb 16 04:06:58,1234"      , 1487239618, 0.1234, -21600},

    /* No year with imezone specified */
    {"no_year_TZ"  , "Feb 16 04:06:58 -0600"     , 1487239618, 0     ,      0},
    {"no_year_N_TZ", "Feb 16 04:06:58.1234 -0600", 1487239618, 0.1234,      0},
    {"no_year_NC_TZ","Feb 16 04:06:58,1234 -0600", 1487239618, 0.1234,      0},

    /* Same date for different timezones, same timestamp */
    {"generic_TZ"   , "07/17/2017 20:17:03 +0000"  , 1500322623, 0,   0},
    {"generic_TZ"   , "07/18/2017 01:47:03 +0530"  , 1500322623, 0,   0},
    {"generic_TZ"   , "07/18/2017 01:47:03 +05:30"  , 1500322623, 0,   0},
    {"generic_TZ"   , "07/18/2017 05:17:03 +0900"  , 1500322623, 0,   0},
    {"generic_TZ"   , "07/17/2017 22:17:03 +0200"  , 1500322623, 0,   0},
    {"generic_N_TZ" , "07/17/2017 22:17:03.1 +0200", 1500322623, 0.1, 0},
    {"generic_N_TZ" , "07/17/2017 22:17:03.1 +02:00", 1500322623, 0.1, 0},
    {"generic_NC_TZ", "07/17/2017 22:17:03,1 +0200",  1500322623, 0.1, 0},
    {"generic_NC_TZ", "07/17/2017 22:17:03,1 +02:00", 1500322623, 0.1, 0},

    /* Same date for different timezones, same timestamp w/ fixed UTC offset */
    {"generic"   , "07/18/2017 01:47:03"   , 1500322623, 0,   19800},
    {"generic"   , "07/18/2017 05:17:03"   , 1500322623, 0,   32400},
    {"generic"   , "07/17/2017 22:17:03"   , 1500322623, 0,    7200},
    {"generic_N" , "07/17/2017 22:17:03.1" , 1500322623, 0.1,  7200},
    {"generic_NC", "07/17/2017 22:17:03,1" , 1500322623, 0.1,  7200},

    /* default UTC: the following timings 'are' in UTC already */
    {"default_UTC"    , "07/17/2017 20:17:03"      , 1500322623, 0     , 0},
    {"default_UTC_Z"  , "07/17/2017 20:17:03Z"     , 1500322623, 0     , 0},
    {"default_UTC_N_Z", "07/17/2017 20:17:03.1234Z", 1500322623, 0.1234, 0},
    {"default_UTC_NC_Z","07/17/2017 20:17:03,1234Z", 1500322623, 0.1234, 0},

    {"apache_error", "Fri Jul 17 20:17:03.1234 2017", 1500322623, 0.1234, 0}
};


int flb_parser_json_do(struct flb_parser *parser,
                       char *buf, size_t length,
                       void **out_buf, size_t *out_size,
                       struct flb_time *out_time);

int flb_parser_regex_do(struct flb_parser *parser,
                        char *buf, size_t length,
                        void **out_buf, size_t *out_size,
                        struct flb_time *out_time);

/* Parse timezone string and get the offset */
void test_parser_tzone_offset()
{
    int i;
    int len;
    int ret;
    int diff;
    struct tz_check *t;

    /* Valid offsets */
    for (i = 0; i < sizeof(tz_entries_ok) / sizeof(struct tz_check); i++) {
        t = &tz_entries_ok[0];
        len = strlen(t->val);

        ret = flb_parser_tzone_offset(t->val, len, &diff);
        TEST_CHECK(ret == 0 && diff == t->diff);
    }

    /* Invalid offsets */
    for (i = 0; i < sizeof(tz_entries_error) / sizeof(struct tz_check); i++) {
        t = &tz_entries_error[0];
        len = strlen(t->val);

        ret = flb_parser_tzone_offset(t->val, len, &diff);
        TEST_CHECK(ret != 0);
    }
}

static void load_json_parsers(struct flb_config *config)
{
    int ret;

    ret = flb_parser_conf_file(JSON_PARSERS, config);
    TEST_CHECK(ret == 0);
}

static void load_regex_parsers(struct flb_config *config)
{
    int ret;

    ret = flb_parser_conf_file(REGEX_PARSERS, config);
    TEST_CHECK(ret == 0);
}

void test_parser_time_lookup()
{
    int i;
    int len;
    int ret;
    int toff;
    int year_diff = 0;
    double ns;
    time_t now;
    time_t epoch;
    struct flb_parser *p;
    struct flb_config *config;
    struct time_check *t;
    struct tm tm;

    /* Dummy config context */
    // config = flb_malloc(sizeof(struct flb_config));
    // mk_list_init(&config->parsers);
    config = flb_config_init();

    load_json_parsers(config);

    /* Iterate tests */
    now = time(NULL);
    for (i = 0; i < sizeof(time_entries) / sizeof(struct time_check); i++) {
        t = &time_entries[i];
        p = flb_parser_get(t->parser_name, config);
        TEST_CHECK(p != NULL);

        if (p == NULL) {
            continue;
        }

        /* Alter time offset if set */
        toff = 0;
        if (t->utc_offset != 0) {
            toff = p->time_offset;
            p->time_offset = t->utc_offset;
        }

        /* Adjust timestamp for parsers using no-year */
        if (p->time_with_year == FLB_FALSE) {
            time_t time_test = t->epoch;
            struct tm tm_now;
            struct tm tm_test;

            gmtime_r(&now, &tm_now);
            gmtime_r(&time_test, &tm_test);

            if (tm_now.tm_year != tm_test.tm_year) {
                year_diff = ((tm_now.tm_year - tm_test.tm_year) * 31536000);
            }
        }
        else {
            year_diff = 0;
        }

        /* Lookup time */
        len = strlen(t->time_string);
        ret = flb_parser_time_lookup(t->time_string, len, now, p, &tm, &ns);
        TEST_CHECK(ret == 0);

        epoch = flb_parser_tm2time(&tm);
        epoch -= year_diff;
        TEST_CHECK(t->epoch == epoch);
        TEST_CHECK(t->frac_seconds == ns);

        if (t->utc_offset != 0) {
            p->time_offset = toff;
        }
    }

    flb_parser_exit(config);
    flb_config_exit(config);
}

/* Do time lookup using the JSON parser backend*/
void test_json_parser_time_lookup()
{
    int i;
    int ret;
    int len;
    int toff;
    int year_diff = 0;
    time_t epoch;
    long nsec;
    char buf[512];
    void *out_buf;
    size_t out_size;
    struct flb_time out_time;
    struct flb_parser *p;
    struct flb_config *config;
    struct time_check *t;

    /* Dummy config context */
    config = flb_config_init();

    /* Load parsers */
    load_json_parsers(config);

    for (i = 0; i < sizeof(time_entries) / sizeof(struct time_check); i++) {
        t = &time_entries[i];
        p = flb_parser_get(t->parser_name, config);
        TEST_CHECK(p != NULL);

        if (p == NULL) {
            continue;
        }

        /* Alter time offset if set */
        toff = 0;
        if (t->utc_offset != 0) {
            toff = p->time_offset;
            p->time_offset = t->utc_offset;
        }

        /* Adjust timestamp for parsers using no-year */
        if (p->time_with_year == FLB_FALSE) {
            time_t time_now = time(NULL);
            time_t time_test = t->epoch;
            struct tm tm_now;
            struct tm tm_test;

            gmtime_r(&time_now, &tm_now);
            gmtime_r(&time_test, &tm_test);

            if (tm_now.tm_year != tm_test.tm_year) {
                year_diff = ((tm_now.tm_year - tm_test.tm_year) * 31536000);
            }
        }
        else {
            year_diff = 0;
        }

        /* Compose the string */
        len = snprintf(buf, sizeof(buf) - 1, JSON_FMT_01, t->time_string);

        /* Invoke the JSON parser backend */
        ret = flb_parser_json_do(p, buf, len, &out_buf, &out_size, &out_time);
        TEST_CHECK(ret != -1);
        TEST_CHECK(out_buf != NULL);

        /* Check time */
        epoch = t->epoch + year_diff;

        TEST_CHECK(out_time.tm.tv_sec == epoch);
        nsec = t->frac_seconds * 1000000000;
        TEST_CHECK(out_time.tm.tv_nsec == nsec);

        if (t->utc_offset != 0) {
            p->time_offset = toff;
        }

        flb_free(out_buf);
    }

    flb_parser_exit(config);
    flb_config_exit(config);
}

/* Do time lookup using the Regex parser backend*/
void test_regex_parser_time_lookup()
{
    int i;
    int ret;
    int len;
    int toff;
    int year_diff = 0;
    time_t epoch;
    long nsec;
    char buf[512];
    void *out_buf;
    size_t out_size;
    struct flb_time out_time;
    struct flb_parser *p;
    struct flb_config *config;
    struct time_check *t;

    /* Dummy config context */
    config = flb_config_init();

    /* Load parsers */
    load_regex_parsers(config);

    for (i = 0; i < sizeof(time_entries) / sizeof(struct time_check); i++) {
        t = &time_entries[i];
        p = flb_parser_get(t->parser_name, config);
        TEST_CHECK(p != NULL);

        if (p == NULL) {
            continue;
        }

        /* Alter time offset if set */
        toff = 0;
        if (t->utc_offset != 0) {
            toff = p->time_offset;
            p->time_offset = t->utc_offset;
        }

        /* Adjust timestamp for parsers using no-year */
        if (p->time_with_year == FLB_FALSE) {
            time_t time_now = time(NULL);
            time_t time_test = t->epoch;
            struct tm tm_now;
            struct tm tm_test;

            gmtime_r(&time_now, &tm_now);
            gmtime_r(&time_test, &tm_test);

            if (tm_now.tm_year != tm_test.tm_year) {
                year_diff = ((tm_now.tm_year - tm_test.tm_year) * 31536000);
            }
        }
        else {
            year_diff = 0;
        }

        /* Compose the string */
        len = snprintf(buf, sizeof(buf) - 1, REGEX_FMT_01, t->time_string);

        /* Invoke the JSON parser backend */
        ret = flb_parser_regex_do(p, buf, len, &out_buf, &out_size, &out_time);
        TEST_CHECK(ret != -1);
        TEST_CHECK(out_buf != NULL);

        /* Adjust time without year */
        epoch = t->epoch + year_diff;

        /* Check time */
        TEST_CHECK(out_time.tm.tv_sec == epoch);
        nsec = t->frac_seconds * 1000000000;
        TEST_CHECK(out_time.tm.tv_nsec == nsec);

        if (t->utc_offset != 0) {
            p->time_offset = toff;
        }

        flb_free(out_buf);
    }

    flb_parser_exit(config);
    flb_config_exit(config);
}


TEST_LIST = {
    { "tzone_offset", test_parser_tzone_offset},
    { "time_lookup", test_parser_time_lookup},
    { "json_time_lookup", test_json_parser_time_lookup},
    { "regex_time_lookup", test_regex_parser_time_lookup},
    { 0 }
};
