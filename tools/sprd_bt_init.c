/*
 * Minimal UWE5622 / Marlin3 Bluetooth vendor initialization test.
 *
 * Sends:
 *   FCA0 - PSKEY
 *   FCA2 - RF parameters
 *   FCA1 - Bluetooth core enable
 *
 * Directly to /dev/ttyBT0 using H4 framing.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <ctype.h>

#define HCI_PSKEY       0xFCA0
#define HCI_CORE_ENABLE 0xFCA1
#define HCI_RF_PARA     0xFCA2

#define H4_COMMAND      0x01
#define H4_EVENT        0x04

#define DEVICE          "/dev/ttyBT0"
#define BT_PSKEY_FILE   "/lib/firmware/bt_configure_pskey.ini"
#define BT_RF_FILE      "/lib/firmware/bt_configure_rf.ini"

static void put_u8(uint8_t **p, uint8_t v)
{
    *(*p)++ = v;
}

static void put_u16(uint8_t **p, uint16_t v)
{
    *(*p)++ = v & 0xff;
    *(*p)++ = (v >> 8) & 0xff;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    *(*p)++ = v & 0xff;
    *(*p)++ = (v >> 8) & 0xff;
    *(*p)++ = (v >> 16) & 0xff;
    *(*p)++ = (v >> 24) & 0xff;
}

static void dump(const char *prefix, const uint8_t *buf, size_t len)
{
    size_t i;

    printf("%s (%zu):", prefix, len);

    for (i = 0; i < len; i++)
        printf(" %02X", buf[i]);

    printf("\n");
}

static int read_byte_timeout(int fd, uint8_t *b, int timeout_ms)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN
    };

    int r;

    r = poll(&pfd, 1, timeout_ms);
    if (r <= 0)
        return -1;

    return read(fd, b, 1) == 1 ? 0 : -1;
}


/*
 * Same basic framing as read_hci_event() from hciattach:
 *
 *   04 event length parameters...
 */
static int read_hci_event(int fd, uint8_t *buf, size_t size)
{
    size_t count;
    size_t remain;
    uint8_t b;

    if (size < 3)
        return -1;

    /*
     * Synchronize to H4 event packet.
     */
    for (;;) {
        if (read_byte_timeout(fd, &b, 3000) < 0) {
            fprintf(stderr, "timeout waiting for HCI event\n");
            return -1;
        }

        if (b == H4_EVENT)
            break;

        fprintf(stderr, "discarding byte %02X while waiting for event\n", b);
    }

    buf[0] = b;

    /*
     * Event code + parameter length.
     */
    if (read_byte_timeout(fd, &buf[1], 1000) < 0 ||
        read_byte_timeout(fd, &buf[2], 1000) < 0)
        return -1;

    remain = buf[2];

    if (remain > size - 3) {
        fprintf(stderr, "HCI event too large: %zu bytes\n", remain);
        return -1;
    }

    count = 3;

    while (remain) {
        ssize_t n = read(fd, buf + count, remain);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("read");
            return -1;
        }

        if (n == 0)
            return -1;

        count += n;
        remain -= n;
    }

    return (int)count;
}


static int send_command(int fd, uint16_t opcode,
                        const uint8_t *payload, size_t payload_len)
{
    uint8_t buf[4 + 255];
    uint8_t event[260];
    size_t len;
    ssize_t n;
    int event_len;

    if (payload_len > 255)
        return -1;

    buf[0] = H4_COMMAND;
    buf[1] = opcode & 0xff;
    buf[2] = (opcode >> 8) & 0xff;
    buf[3] = payload_len;

    memcpy(buf + 4, payload, payload_len);

    len = payload_len + 4;

    dump("TX", buf, len);

    n = write(fd, buf, len);
    if (n != (ssize_t)len) {
        if (n < 0)
            perror("write");
        else
            fprintf(stderr, "short write: %zd/%zu\n", n, len);

        return -1;
    }

    event_len = read_hci_event(fd, event, sizeof(event));
    if (event_len < 0)
        return -1;

    dump("RX", event, event_len);

    return 0;
}


/*
 * OrangePi _2 variant derives the Bluetooth address from wlan0:
 *
 * wlan0 50:37:cd:7f:18:1c
 *
 * PSKEY byte order:
 *       1c 18 7f cd 37 50
 *
 * then addr[0]++, addr[1]++:
 *       1d 19 7f cd 37 50
 */
static int get_bt_address(uint8_t addr[6])
{
    FILE *f;
    unsigned int mac[6];
    int i;

    f = fopen("/sys/class/net/wlan0/address", "r");
    if (!f) {
        perror("/sys/class/net/wlan0/address");
        return -1;
    }

    if (fscanf(f, "%x:%x:%x:%x:%x:%x",
               &mac[0], &mac[1], &mac[2],
               &mac[3], &mac[4], &mac[5]) != 6) {
        fprintf(stderr, "cannot parse wlan0 MAC\n");
        fclose(f);
        return -1;
    }

    fclose(f);

    for (i = 0; i < 6; i++)
        addr[5 - i] = (uint8_t)mac[i];

    addr[0]++;
    addr[1]++;

    printf("PSKEY BT address:");
    for (i = 0; i < 6; i++)
        printf(" %02X", addr[i]);
    printf("\n");

    return 0;
}



#define MAX_INI_ENTRIES 64
#define MAX_INI_VALUES  16

struct ini_entry {
    char key[64];
    uint32_t value[MAX_INI_VALUES];
    size_t count;
};

struct ini_config {
    struct ini_entry entry[MAX_INI_ENTRIES];
    size_t count;
};

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static int load_ini(const char *path, struct ini_config *cfg)
{
    FILE *f;
    char linebuf[2048];
    unsigned line = 0;

    memset(cfg, 0, sizeof(*cfg));
    f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    while (fgets(linebuf, sizeof(linebuf), f)) {
        char *s, *eq, *p;
        struct ini_entry *e;

        line++;
        s = trim(linebuf);
        if (!*s || *s == '#')
            continue;

        eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s:%u: missing '='\n", path, line);
            fclose(f);
            return -1;
        }

        *eq++ = '\0';
        s = trim(s);
        eq = trim(eq);

        if (!*s || cfg->count >= MAX_INI_ENTRIES ||
            strlen(s) >= sizeof(cfg->entry[0].key)) {
            fprintf(stderr, "%s:%u: invalid key\n", path, line);
            fclose(f);
            return -1;
        }

        e = &cfg->entry[cfg->count++];
        strcpy(e->key, s);
        p = eq;

        while (*p) {
            char *end;
            unsigned long v;

            if (e->count >= MAX_INI_VALUES) {
                fprintf(stderr, "%s:%u: too many values for %s\n",
                        path, line, e->key);
                fclose(f);
                return -1;
            }

            errno = 0;
            v = strtoul(p, &end, 0);
            if (errno || end == p || v > UINT32_MAX) {
                fprintf(stderr, "%s:%u: invalid value for %s\n",
                        path, line, e->key);
                fclose(f);
                return -1;
            }

            e->value[e->count++] = (uint32_t)v;
            p = end;
            while (isspace((unsigned char)*p)) p++;

            if (!*p)
                break;
            if (*p != ',') {
                fprintf(stderr, "%s:%u: expected ',' for %s\n",
                        path, line, e->key);
                fclose(f);
                return -1;
            }
            p++;
            while (isspace((unsigned char)*p)) p++;
        }

        if (!e->count) {
            fprintf(stderr, "%s:%u: no value for %s\n", path, line, e->key);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    printf("Loaded %zu entries from %s\n", cfg->count, path);
    return 0;
}

static const struct ini_entry *ini_get(const struct ini_config *cfg,
                                       const char *key, size_t min_count,
                                       size_t max_count)
{
    size_t i;
    for (i = 0; i < cfg->count; i++) {
        if (!strcmp(cfg->entry[i].key, key)) {
            if (cfg->entry[i].count < min_count ||
                cfg->entry[i].count > max_count) {
                fprintf(stderr, "%s: expected %zu..%zu values, got %zu\n",
                        key, min_count, max_count, cfg->entry[i].count);
                return NULL;
            }
            return &cfg->entry[i];
        }
    }
    fprintf(stderr, "missing INI key: %s\n", key);
    return NULL;
}

static int put_ini8(uint8_t **p, const struct ini_config *c,
                    const char *key, size_t n)
{
    const struct ini_entry *e = ini_get(c, key, n, n);
    size_t i;
    if (!e) return -1;
    for (i=0;i<n;i++) {
        if (e->value[i] > UINT8_MAX) return -1;
        put_u8(p,(uint8_t)e->value[i]);
    }
    return 0;
}

static int put_ini16(uint8_t **p, const struct ini_config *c,
                     const char *key, size_t n)
{
    const struct ini_entry *e = ini_get(c, key, n, n);
    size_t i;
    if (!e) return -1;
    for (i=0;i<n;i++) {
        if (e->value[i] > UINT16_MAX) return -1;
        put_u16(p,(uint16_t)e->value[i]);
    }
    return 0;
}

static int put_ini32(uint8_t **p, const struct ini_config *c,
                     const char *key, size_t min_n, size_t max_n)
{
    const struct ini_entry *e = ini_get(c, key, min_n, max_n);
    size_t i;
    if (!e) return -1;
    for (i=0;i<e->count;i++) put_u32(p,e->value[i]);
    return (int)e->count;
}

static size_t build_pskey(uint8_t *buf, const struct ini_config *c,
                          const uint8_t addr[6])
{
    uint8_t *p=buf;
    const struct ini_entry *e;
    int n;
    size_t i;

#define P8(k,n)  do { if (put_ini8(&p,c,k,n)<0) return 0; } while(0)
#define P16(k,n) do { if (put_ini16(&p,c,k,n)<0) return 0; } while(0)
#define P32(k,n) do { if (put_ini32(&p,c,k,n,n)<0) return 0; } while(0)

    P32("device_class",1);
    P8("feature_set",16);

    e=ini_get(c,"device_addr",6,6);
    if (!e) return 0;              /* require field, but override its value */
    for(i=0;i<6;i++) put_u8(&p,addr[i]);

    P16("comp_id",1);
    P8("g_sys_uart0_communication_supported",1);
    P8("cp2_log_mode",1);
    P8("LogLevel",1);
    P8("g_central_or_perpheral",1);
    P16("Log_BitMask",1);
    P8("super_ssp_enable",1);
    P8("common_rfu_b3",1);
    P32("common_rfu_w",2);
    P32("le_rfu_w",2);
    P32("lmp_rfu_w",2);
    P32("lc_rfu_w",2);
    P16("g_wbs_nv_117",1);
    P16("g_wbs_nv_118",1);
    P16("g_nbv_nv_117",1);
    P16("g_nbv_nv_118",1);
    P8("g_sys_sco_transmit_mode",1);
    P8("audio_rfu_b1",1);
    P8("audio_rfu_b2",1);
    P8("audio_rfu_b3",1);
    P32("audio_rfu_w",2);
    P8("g_sys_sleep_in_standby_supported",1);
    P8("g_sys_sleep_master_supported",1);
    P8("g_sys_sleep_slave_supported",1);
    P8("power_rfu_b1",1);
    P32("power_rfu_w",2);
    P32("win_ext",1);
    P8("edr_tx_edr_delay",1);
    P8("edr_rx_edr_delay",1);
    P8("tx_delay",1);
    P8("rx_delay",1);
    P32("bb_rfu_w",2);
    P8("agc_mode",1);
    P8("diff_or_eq",1);
    P8("ramp_mode",1);
    P8("modem_rfu_b1",1);
    P32("modem_rfu_w",2);
    P32("BQB_BitMask_1",1);
    P32("BQB_BitMask_2",1);
    P16("bt_coex_threshold",8);

    n=put_ini32(&p,c,"other_rfu_w",2,6);
    if(n<0) return 0;
    while(n++<6) put_u32(&p,0);   /* original struct was memset(0) */

#undef P8
#undef P16
#undef P32
    return p-buf;
}

static size_t build_rf(uint8_t *buf, const struct ini_config *c)
{
    uint8_t *p=buf;
#define R8(k,n)  do { if (put_ini8(&p,c,k,n)<0) return 0; } while(0)
#define R16(k,n) do { if (put_ini16(&p,c,k,n)<0) return 0; } while(0)
#define R32(k,n) do { if (put_ini32(&p,c,k,n,n)<0) return 0; } while(0)

    R16("g_GainValue_A",6);
    R16("g_ClassicPowerValue_A",10);
    R16("g_LEPowerValue_A",16);
    R16("g_BRChannelpwrvalue_A",8);
    R16("g_EDRChannelpwrvalue_A",8);
    R16("g_LEChannelpwrvalue_A",8);
    R16("g_GainValue_B",6);
    R16("g_ClassicPowerValue_B",10);
    R16("g_LEPowerValue_B",16);
    R16("g_BRChannelpwrvalue_B",8);
    R16("g_EDRChannelpwrvalue_B",8);
    R16("g_LEChannelpwrvalue_B",8);
    R16("LE_fix_powerword",1);
    R8("Classic_pc_by_channel",1);
    R8("LE_pc_by_channel",1);
    R8("RF_switch_mode",1);
    R8("Data_Capture_Mode",1);
    R8("Analog_IQ_Debug_Mode",1);
    R8("RF_common_rfu_b3",1);
    R32("RF_common_rfu_w",5);

#undef R8
#undef R16
#undef R32
    return p-buf;
}


static int setup_tty(int fd)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&tio);

    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);

    tio.c_cflag |= CLOCAL | CREAD;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        return -1;
    }

    tcflush(fd, TCIOFLUSH);

    return 0;
}


int main(void)
{
    int fd;
    uint8_t pskey[255];
    uint8_t rf[255];
    uint8_t addr[6];
    uint8_t core_enable[] = { 0x00, 0x00, 0x01 };
    struct ini_config pskey_cfg;
    struct ini_config rf_cfg;

    size_t pskey_len;
    size_t rf_len;

    if (load_ini(BT_PSKEY_FILE, &pskey_cfg) < 0)
        return 1;

    if (load_ini(BT_RF_FILE, &rf_cfg) < 0)
        return 1;

    if (get_bt_address(addr) < 0)
        return 1;

    pskey_len = build_pskey(pskey, &pskey_cfg, addr);
    rf_len = build_rf(rf, &rf_cfg);

    printf("PSKEY payload length: %zu\n", pskey_len);
    printf("RF payload length:    %zu\n", rf_len);

    if (pskey_len != 176) {
        fprintf(stderr,
                "ERROR: expected PSKEY length 176 got %zu\n",
                pskey_len);
        return 1;
    }

    if (rf_len != 252) {
        fprintf(stderr,
                "ERROR: expected RF length 252, got %zu\n",
                rf_len);
        return 1;
    }

    fd = open(DEVICE, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(DEVICE);
        return 1;
    }

    if (setup_tty(fd) < 0) {
        close(fd);
        return 1;
    }

    printf("\n--- FCA0 PSKEY ---\n");
    if (send_command(fd, HCI_PSKEY, pskey, pskey_len) < 0)
        goto fail;

    printf("\n--- FCA2 RF parameters ---\n");
    if (send_command(fd, HCI_RF_PARA, rf, rf_len) < 0)
        goto fail;

    printf("\n--- FCA1 core enable ---\n");
    if (send_command(fd, HCI_CORE_ENABLE,
                     core_enable, sizeof(core_enable)) < 0)
        goto fail;

    printf("\nInitialization sequence completed.\n");

    close(fd);
    return 0;

fail:
    fprintf(stderr, "\nInitialization failed.\n");
    close(fd);
    return 1;
} 
