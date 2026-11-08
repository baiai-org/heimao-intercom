package net.openradiolink.app

import org.json.JSONObject

/**
 * One row of the public direct peer list, as reported by /api/status.
 * Fields mirror the firmware's np2p.peers objects (netp2pnet.c netp2pnet_status_json);
 * the panel's renderNp2p() is the reference for what each value means.
 */
internal data class PeerInfo(
    val name: String,
    val address: String,
    val state: String,
    val rttMs: Int,
    val rx: Int,
    val tx: Int,
    val drop: Int,
) {
    val stateText: String get() = when (state) {
        "linked" -> "已连通"
        "knocking" -> "敲击中"
        "expired" -> "已过期"
        else -> state.ifBlank { "未知" }
    }
}

/**
 * The APP's read-only view of NET-P2P public direct mode.
 *
 * Pure data, Android-free: the Compose layer only renders it and the unit tests
 * only construct it, the same way the web control panel keeps renderNp2p() a thin
 * function of the /api/status document. No command logic lives here.
 */
internal data class Np2pState(
    /** false when the board dropped the whole np2p section (status buffer overflow); the page shows a placeholder. */
    val available: Boolean,
    /** false before STUN resolves: pub 0.0.0.0 / port 0. */
    val ready: Boolean,
    /** "222.182.14.49:7959" — the exact string the peer pastes into its own panel. */
    val ipv4Text: String,
    /** "[2408:...]:54321", or "" when the router gave no RA. */
    val ipv6Text: String,
    val natText: String,
    /** true once the STUN result is older than STALE_AFTER_SECONDS; a peer using it may fail. */
    val stale: Boolean,
    val stunAgeSeconds: Long,
    val rttMs: Int,
    val hub: Boolean,
    val hubOffline: Boolean,
    val hubQuietSeconds: Long,
    val peers: List<PeerInfo>,
) {
    val linkedCount: Int get() = peers.count { it.state == "linked" }
    val peerCount: Int get() = peers.size

    /** The section header line, in the panel's "连通 n/m" spirit. */
    fun onlineCount(): String = "$linkedCount/$peerCount"

    companion object {
        const val STALE_AFTER_SECONDS = 30L
        const val V6_PORT = 54321

        /**
         * wifi defaults to the wifi object *inside* /api/status, which is where
         * the board publishes ip6.  The separate /api/wifi endpoint only carries
         * saved profiles and link state, so passing SessionView.wifi here would
         * silently hide every IPv6 address.
         */
        fun parse(status: JSONObject, wifi: JSONObject = status.optJSONObject("wifi") ?: JSONObject()): Np2pState {
            val np = status.optJSONObject("np2p")
            if (np == null) {
                // net_http.c drops the section whole when the status body is too
                // large; the UI must say so instead of pretending there are no peers.
                return Np2pState(false, false, "", "", "", false, -1, -1, false, false, 0, emptyList())
            }
            val pub = np.optString("pub")
            val port = np.optInt("port", 0)
            val ready = pub.isNotEmpty() && pub != "0.0.0.0" && port != 0
            val v6 = wifi.optString("ip6").trim()
            val natText = when (np.optString("nat", "unknown")) {
                "port-preserving" -> "端口保持 NAT"
                "not-port-preserving" -> "非端口保持 NAT"
                else -> "NAT 未知"
            }
            val age = np.optLong("stun_age_s", -1)
            val peers = buildList {
                val list = np.optJSONArray("peers") ?: return@buildList
                for (i in 0 until list.length()) {
                    val n = list.optJSONObject(i) ?: continue
                    val ip = n.optString("ip")
                    val p = n.optInt("port", V6_PORT)
                    // IPv6 literals need brackets so the text stays one paste-ready run.
                    val address = if (ip.indexOf(':') > 0) "[$ip]:$p" else "$ip:$p"
                    add(PeerInfo(
                        name = n.optString("name").ifBlank { "未命名" },
                        address = address,
                        state = n.optString("state"),
                        rttMs = n.optInt("rtt_ms", -1),
                        rx = n.optInt("rx", 0),
                        tx = n.optInt("tx", 0),
                        drop = n.optInt("drop", 0),
                    ))
                }
            }
            return Np2pState(
                available = true,
                ready = ready,
                ipv4Text = if (ready) "$pub:$port" else "",
                ipv6Text = if (v6.isNotEmpty() && v6 != "::") "[$v6]:$V6_PORT" else "",
                natText = natText,
                stale = age > STALE_AFTER_SECONDS,
                stunAgeSeconds = if (age >= 0) age else -1,
                rttMs = np.optInt("rtt_ms", -1),
                hub = np.optBoolean("hub", false),
                hubOffline = np.optBoolean("hub_offline", false),
                hubQuietSeconds = np.optLong("hub_quiet_s", 0),
                peers = peers,
            )
        }
    }
}
