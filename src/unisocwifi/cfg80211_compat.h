#ifndef __SPRDWL_CFG80211_COMPAT_H__
#define __SPRDWL_CFG80211_COMPAT_H__

#include <linux/bitops.h>
#include <net/cfg80211.h>

/*
 * OpenWrt 24.10 builds cfg80211 from the 6.12 backports tree.
 * cfg80211_get_bss() is a static inline wrapper there; the exported
 * symbol is __cfg80211_get_bss().
 */
struct cfg80211_bss *__cfg80211_get_bss(
	struct wiphy *wiphy,
	struct ieee80211_channel *channel,
	const u8 *bssid,
	const u8 *ssid, size_t ssid_len,
	enum ieee80211_bss_type bss_type,
	enum ieee80211_privacy privacy,
	u32 use_for);

#define SPRDWL_BSS_USE_FOR_NORMAL	BIT(0)

#endif 
