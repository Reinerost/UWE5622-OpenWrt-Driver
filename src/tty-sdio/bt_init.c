#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "bt_init.h"
#include "woble.h"

#define HCI_PSKEY              0xFCA0
#define HCI_CORE_ENABLE        0xFCA1
#define HCI_RF_PARA            0xFCA2

#define BT_PSKEY_FILE          "bt_configure_pskey.ini"
#define BT_RF_FILE             "bt_configure_rf.ini"

#define BT_PSKEY_LEN           176
#define BT_RF_LEN              252

#define MAX_INI_ENTRIES        64
#define MAX_INI_VALUES         16

struct ini_entry {
	char key[64];
	u32 value[MAX_INI_VALUES];
	size_t count;
};

struct ini_config {
	struct ini_entry entry[MAX_INI_ENTRIES];
	size_t count;
};

static char *trim(char *s)
{
	char *e;

	while (*s && (*s == ' ' || *s == '\t' ||
		*s == '\r' || *s == '\n'))
		s++;

	if (!*s)
		return s;

	e = s + strlen(s) - 1;

	while (e >= s &&
		(*e == ' ' || *e == '\t' ||
		*e == '\r' || *e == '\n'))
		*e-- = '\0';

	return s;
}

static int parse_ini(const struct firmware *fw, struct ini_config *cfg)
{
	char *buf;
	char *cur;
	char *line;
	int ret = 0;

	buf = kmemdup_nul(fw->data, fw->size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	cur = buf;

	while ((line = strsep(&cur, "\n")) != NULL) {
		struct ini_entry *entry;
		char *s;
		char *eq;
		char *values;

		s = trim(line);

		if (!*s || *s == '#')
			continue;

		eq = strchr(s, '=');
		if (!eq) {
			ret = -EINVAL;
			goto out;
		}

		*eq++ = '\0';

		s = trim(s);
		values = trim(eq);

		if (!*s || !*values ||
			cfg->count >= MAX_INI_ENTRIES ||
			strlen(s) >= sizeof(cfg->entry[0].key)) {
			ret = -EINVAL;
		goto out;
			}

			entry = &cfg->entry[cfg->count++];
			strscpy(entry->key, s, sizeof(entry->key));

			while (*values) {
				char *comma;
				char *value;
				u32 v;

				if (entry->count >= MAX_INI_VALUES) {
					ret = -E2BIG;
					goto out;
				}

				comma = strchr(values, ',');
				if (comma)
					*comma = '\0';

				value = trim(values);

				ret = kstrtou32(value, 0, &v);
				if (ret)
					goto out;

				entry->value[entry->count++] = v;

				if (!comma)
					break;

				values = trim(comma + 1);
			}

			if (!entry->count) {
				ret = -EINVAL;
				goto out;
			}
	}

	out:
	kfree(buf);
	return ret;
}

static const struct ini_entry *
ini_get(const struct ini_config *cfg, const char *key,
		size_t min_count, size_t max_count)
{
	size_t i;

	for (i = 0; i < cfg->count; i++) {
		const struct ini_entry *entry = &cfg->entry[i];

		if (strcmp(entry->key, key))
			continue;

		if (entry->count < min_count ||
			entry->count > max_count)
			return NULL;

		return entry;
	}

	return NULL;
}

static void put_u8(u8 **p, u8 v)
{
	*(*p)++ = v;
}

static void put_u16(u8 **p, u16 v)
{
	*(*p)++ = v & 0xff;
	*(*p)++ = (v >> 8) & 0xff;
}

static void put_u32(u8 **p, u32 v)
{
	*(*p)++ = v & 0xff;
	*(*p)++ = (v >> 8) & 0xff;
	*(*p)++ = (v >> 16) & 0xff;
	*(*p)++ = (v >> 24) & 0xff;
}

static int put_ini8(u8 **p, const struct ini_config *cfg,
					const char *key, size_t n)
{
	const struct ini_entry *entry;
	size_t i;

	entry = ini_get(cfg, key, n, n);
	if (!entry)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		if (entry->value[i] > U8_MAX)
			return -ERANGE;

		put_u8(p, entry->value[i]);
	}

	return 0;
}

static int put_ini16(u8 **p, const struct ini_config *cfg,
					 const char *key, size_t n)
{
	const struct ini_entry *entry;
	size_t i;

	entry = ini_get(cfg, key, n, n);
	if (!entry)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		if (entry->value[i] > U16_MAX)
			return -ERANGE;

		put_u16(p, entry->value[i]);
	}

	return 0;
}

static int put_ini32(u8 **p, const struct ini_config *cfg,
					 const char *key, size_t min_n, size_t max_n)
{
	const struct ini_entry *entry;
	size_t i;

	entry = ini_get(cfg, key, min_n, max_n);
	if (!entry)
		return -EINVAL;

	for (i = 0; i < entry->count; i++)
		put_u32(p, entry->value[i]);

	return entry->count;
}

static size_t build_pskey(u8 *buf, const struct ini_config *cfg)
{
	u8 *p = buf;
	int n;

	#define P8(k, n)                                                       \
	do {                                                           \
		if (put_ini8(&p, cfg, k, n) < 0)                      \
			return 0;                                      \
	} while (0)

	#define P16(k, n)                                                      \
	do {                                                           \
		if (put_ini16(&p, cfg, k, n) < 0)                     \
			return 0;                                      \
	} while (0)

	#define P32(k, n)                                                      \
	do {                                                           \
		if (put_ini32(&p, cfg, k, n, n) < 0)                  \
			return 0;                                      \
	} while (0)

	P32("device_class", 1);
	P8("feature_set", 16);

	/*
	 * For the first in-kernel test use device_addr exactly as
	 * supplied by bt_configure_pskey.ini.
	 */
	P8("device_addr", 6);

	P16("comp_id", 1);
	P8("g_sys_uart0_communication_supported", 1);
	P8("cp2_log_mode", 1);
	P8("LogLevel", 1);
	P8("g_central_or_perpheral", 1);
	P16("Log_BitMask", 1);
	P8("super_ssp_enable", 1);
	P8("common_rfu_b3", 1);
	P32("common_rfu_w", 2);
	P32("le_rfu_w", 2);
	P32("lmp_rfu_w", 2);
	P32("lc_rfu_w", 2);
	P16("g_wbs_nv_117", 1);
	P16("g_wbs_nv_118", 1);
	P16("g_nbv_nv_117", 1);
	P16("g_nbv_nv_118", 1);
	P8("g_sys_sco_transmit_mode", 1);
	P8("audio_rfu_b1", 1);
	P8("audio_rfu_b2", 1);
	P8("audio_rfu_b3", 1);
	P32("audio_rfu_w", 2);
	P8("g_sys_sleep_in_standby_supported", 1);
	P8("g_sys_sleep_master_supported", 1);
	P8("g_sys_sleep_slave_supported", 1);
	P8("power_rfu_b1", 1);
	P32("power_rfu_w", 2);
	P32("win_ext", 1);
	P8("edr_tx_edr_delay", 1);
	P8("edr_rx_edr_delay", 1);
	P8("tx_delay", 1);
	P8("rx_delay", 1);
	P32("bb_rfu_w", 2);
	P8("agc_mode", 1);
	P8("diff_or_eq", 1);
	P8("ramp_mode", 1);
	P8("modem_rfu_b1", 1);
	P32("modem_rfu_w", 2);
	P32("BQB_BitMask_1", 1);
	P32("BQB_BitMask_2", 1);
	P16("bt_coex_threshold", 8);

	n = put_ini32(&p, cfg, "other_rfu_w", 2, 6);
	if (n < 0)
		return 0;

	/*
	 * The tested userspace implementation serializes a 176-byte
	 * PSKEY structure.  Missing RFU words are zero-filled.
	 */
	while (n++ < 6)
		put_u32(&p, 0);

	#undef P8
	#undef P16
	#undef P32

	return p - buf;
}

static size_t build_rf(u8 *buf, const struct ini_config *cfg)
{
	u8 *p = buf;

	#define R8(k, n)                                                       \
	do {                                                           \
		if (put_ini8(&p, cfg, k, n) < 0)                      \
			return 0;                                      \
	} while (0)

	#define R16(k, n)                                                      \
	do {                                                           \
		if (put_ini16(&p, cfg, k, n) < 0)                     \
			return 0;                                      \
	} while (0)

	#define R32(k, n)                                                      \
	do {                                                           \
		if (put_ini32(&p, cfg, k, n, n) < 0)                  \
			return 0;                                      \
	} while (0)

	R16("g_GainValue_A", 6);
	R16("g_ClassicPowerValue_A", 10);
	R16("g_LEPowerValue_A", 16);
	R16("g_BRChannelpwrvalue_A", 8);
	R16("g_EDRChannelpwrvalue_A", 8);
	R16("g_LEChannelpwrvalue_A", 8);

	R16("g_GainValue_B", 6);
	R16("g_ClassicPowerValue_B", 10);
	R16("g_LEPowerValue_B", 16);
	R16("g_BRChannelpwrvalue_B", 8);
	R16("g_EDRChannelpwrvalue_B", 8);
	R16("g_LEChannelpwrvalue_B", 8);

	R16("LE_fix_powerword", 1);
	R8("Classic_pc_by_channel", 1);
	R8("LE_pc_by_channel", 1);
	R8("RF_switch_mode", 1);
	R8("Data_Capture_Mode", 1);
	R8("Analog_IQ_Debug_Mode", 1);
	R8("RF_common_rfu_b3", 1);
	R32("RF_common_rfu_w", 5);

	#undef R8
	#undef R16
	#undef R32

	return p - buf;
}

static int send_vendor_cmd(u16 opcode, const u8 *data, size_t len)
{
	struct HC_BT_HDR *payload;
	int ret;

	if (len > U8_MAX)
		return -EINVAL;

	payload = kzalloc(sizeof(*payload) + len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	payload->len = len;
	memcpy(payload->data, data, len);

	ret = hci_cmd_send_sync(opcode, payload, NULL);

	kfree(payload);
	return ret;
}

static int load_config(struct device *dev, const char *name,
					   struct ini_config *cfg)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, name, dev);
	if (ret) {
		dev_err(dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	ret = parse_ini(fw, cfg);
	release_firmware(fw);

	if (ret)
		dev_err(dev, "failed to parse %s: %d\n", name, ret);

	return ret;
}

int sprd_bt_init(struct device *dev)
{
	struct ini_config *cfg;
	u8 *payload;
	size_t len;
	int ret;

	dev_info(dev, "starting UWE5622 Bluetooth vendor initialization\n");

	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	payload = kzalloc(BT_RF_LEN, GFP_KERNEL);
	if (!payload) {
		ret = -ENOMEM;
		goto out_cfg;
	}

	ret = load_config(dev, BT_PSKEY_FILE, cfg);
	if (ret)
		goto out;

	len = build_pskey(payload, cfg);
	if (len != BT_PSKEY_LEN) {
		dev_err(dev, "invalid PSKEY length: %zu\n", len);
		ret = -EINVAL;
		goto out;
	}

	dev_info(dev, "sending PSKEY (%zu bytes)\n", len);

	ret = send_vendor_cmd(HCI_PSKEY, payload, len);
	if (ret)
		goto out;

	memset(cfg, 0, sizeof(*cfg));
	memset(payload, 0, BT_RF_LEN);

	ret = load_config(dev, BT_RF_FILE, cfg);
	if (ret)
		goto out;

	len = build_rf(payload, cfg);
	if (len != BT_RF_LEN) {
		dev_err(dev, "invalid RF parameter length: %zu\n", len);
		ret = -EINVAL;
		goto out;
	}

	dev_info(dev, "sending RF parameters (%zu bytes)\n", len);

	ret = send_vendor_cmd(HCI_RF_PARA, payload, len);
	if (ret)
		goto out;

	payload[0] = 0x00;
	payload[1] = 0x00;
	payload[2] = 0x01;

	dev_info(dev, "enabling Bluetooth core\n");

	ret = send_vendor_cmd(HCI_CORE_ENABLE, payload, 3);
	if (ret)
		goto out;

	dev_info(dev, "UWE5622 Bluetooth vendor initialization complete\n");

	out:
	kfree(payload);
	out_cfg:
	kfree(cfg);

	return ret;
}
