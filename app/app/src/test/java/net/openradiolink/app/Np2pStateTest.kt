package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class Np2pStateTest {
    @Test fun forwardedMemberHasAPathAndNoFakeDirectEndpoint() {
        val np=Np2pState.parse(JSONObject("""{"np2p":{"hub_role":"member","hub_conflict":false,"peer_limit":16,"peers":[
            {"name":"A","ip":"192.0.2.9","port":54321,"state":"linked","hub":true},
            {"name":"C","ip":"0.0.0.0","port":0,"state":"linked","route":"hub","via":"A"}
        ]}}"""))
        assertEquals("1/1",np.onlineCount());assertEquals(1,np.indirectCount)
        assertEquals("经 A 转发 · 无直连地址",np.peers[1].address)
        assertEquals("最近收到语音",np.peers[1].stateText)
        assertEquals("本机连接群聊枢纽",np.roleText)
        assertTrue(np.peers[0].isHub);assertFalse(np.hubConflict)
    }
    @Test fun unresolvedOldPeerNeverDisplaysAnUnusableEndpoint() {
        val np=Np2pState.parse(JSONObject("""{"np2p":{"hub":true,"hub_conflict":true,"peers":[
            {"ip":"0.0.0.0","port":0},{"ip":"","port":54321},{"ip":"::","port":0}
        ]}}"""))
        assertTrue(np.peers.all{it.address=="等待握手取得地址"})
        assertEquals("本机是群聊枢纽",np.roleText);assertTrue(np.hubConflict)
    }
    @Test fun sixteenPeersAndCapacityAreNotClipped() {
        val entries=org.json.JSONArray()
        repeat(16){i->entries.put(JSONObject().put("name","p$i").put("ip","2001:db8::$i").put("port",54321).put("state","linked"))}
        val st=JSONObject().put("np2p",JSONObject().put("peer_limit",16).put("peer_count",16).put("peers",entries))
        val np=Np2pState.parse(st)
        assertEquals(16,np.peerLimit);assertEquals(16,np.storedCount);assertEquals(16,np.peers.size)
        assertEquals("[2001:db8::15]:54321",np.peers.last().address)
    }
    @Test fun capacityFallbackAndIncompleteListRemainVisible() {
        assertEquals(4,Np2pState.parse(JSONObject().put("np2p",JSONObject())).peerLimit)
        val np=Np2pState.parse(JSONObject("""{"np2p":{"peer_limit":16,"peer_count":16,"peers":[]}}"""))
        assertEquals(16,np.storedCount);assertEquals(0,np.peers.size)
    }
    private fun status(np: String, wifiIp6: String = ""): Pair<JSONObject, JSONObject> =
        JSONObject(np) to JSONObject().put("ip6", wifiIp6)

    @Test fun readyAddressIsOnePasteReadyRun() {
        val (st, wifi) = status("""{"np2p":{"pub":"222.182.14.49","port":7959}}""")
        val np = Np2pState.parse(st, wifi)
        assertTrue(np.available)
        assertTrue(np.ready)
        assertEquals("222.182.14.49:7959", np.ipv4Text)
        assertEquals("", np.ipv6Text)
    }

    @Test fun ipv6AddressIsBracketedOnOneLine() {
        val (st, wifi) = status(
            """{"np2p":{"pub":"1.2.3.4","port":54321}}""",
            "2408:8207:1c2f:30c1:9f3a:8a6c:2d4b:9f3a")
        val np = Np2pState.parse(st, wifi)
        assertEquals("[2408:8207:1c2f:30c1:9f3a:8a6c:2d4b:9f3a]:54321", np.ipv6Text)
        assertFalse(np.ipv6Text.contains(' '))
    }

    @Test fun ipv6ComesFromTheStatusWifiBlock() {
        // ip6 lives in /api/status wifi; /api/wifi only carries profiles and has no ip6.
        val st = JSONObject("""{"wifi":{"ip6":"2408:8207::9"},"np2p":{"pub":"1.2.3.4","port":54321}}""")
        val np = Np2pState.parse(st)
        assertEquals("[2408:8207::9]:54321", np.ipv6Text)
    }

    @Test fun unroutedAddressReadsAsNotReady() {
        val (st, wifi) = status("""{"np2p":{"pub":"0.0.0.0","port":0}}""")
        val np = Np2pState.parse(st, wifi)
        assertTrue(np.available)
        assertFalse(np.ready)
        assertEquals("", np.ipv4Text)
    }

    @Test fun staleStunResultIsFlagged() {
        val fresh = Np2pState.parse(status("""{"np2p":{"pub":"1.2.3.4","port":1,"stun_age_s":12,"nat":"port-preserving"}}""").first,
            JSONObject())
        assertFalse(fresh.stale)
        assertEquals(12L, fresh.stunAgeSeconds)
        assertEquals("端口保持 NAT", fresh.natText)

        val stale = Np2pState.parse(status("""{"np2p":{"pub":"1.2.3.4","port":1,"stun_age_s":42,"nat":"port-preserving"}}""").first,
            JSONObject())
        assertTrue(stale.stale)
        val notPreserving = Np2pState.parse(status("""{"np2p":{"pub":"1.2.3.4","port":1,"nat":"not-port-preserving"}}""").first,
            JSONObject())
        assertEquals("非端口保持 NAT", notPreserving.natText)
        val unknown = Np2pState.parse(status("""{"np2p":{"pub":"1.2.3.4","port":1}}""").first, JSONObject())
        assertEquals("NAT 未知", unknown.natText)
    }

    @Test fun peerStatesAreTranslatedAndCounted() {
        val (st, wifi) = status("""{"np2p":{"pub":"1.2.3.4","port":1,"rtt_ms":8,"hub":true,"peers":[
            {"name":"测试","ip":"192.0.2.198","port":54321,"state":"linked","rtt_ms":8,"rx":120,"tx":118,"drop":0},
            {"name":"","ip":"192.0.2.13","port":54321,"state":"knocking","rtt_ms":-1,"rx":0,"tx":3,"drop":0},
            {"name":"旧端","ip":"5.6.7.8","port":54321,"state":"expired","rtt_ms":-1,"rx":1,"tx":1,"drop":9}
        ]}}""")
        val np = Np2pState.parse(st, wifi)
        assertEquals(3, np.peerCount)
        assertEquals(1, np.linkedCount)
        assertEquals("1/3", np.onlineCount())
        assertEquals("测试", np.peers[0].name)
        assertEquals("192.0.2.198:54321", np.peers[0].address)
        assertEquals("已连通", np.peers[0].stateText)
        assertEquals("未命名", np.peers[1].name)
        assertEquals("敲击中", np.peers[1].stateText)
        assertEquals("已过期", np.peers[2].stateText)
    }

    @Test fun ipv6PeersAreBracketedForPaste() {
        val (st, wifi) = status("""{"np2p":{"pub":"1.2.3.4","port":1,"peers":[
            {"name":"v6","ip":"2408:8207::9","port":54321,"state":"knocking"}]}}""")
        val np = Np2pState.parse(st, wifi)
        assertEquals("[2408:8207::9]:54321", np.peers[0].address)
    }

    @Test fun droppedSectionShowsPlaceholderInsteadOfNothing() {
        // net_http.c drops the whole np2p section when the status body overflows;
        // the UI must say so rather than silently claim an empty peer list.
        val (st, wifi) = status("{}")
        val np = Np2pState.parse(st, wifi)
        assertFalse(np.available)
        assertFalse(np.ready)
        assertEquals(0, np.peerCount)
    }

    @Test fun hubStateAndOfflineQuietAreReported() {
        val (st, wifi) = status("""{"np2p":{"pub":"1.2.3.4","port":1,"hub":true,"hub_offline":true,"hub_quiet_s":12}}""")
        val np = Np2pState.parse(st, wifi)
        assertTrue(np.hub)
        assertTrue(np.hubOffline)
        assertEquals(12L, np.hubQuietSeconds)
    }
}
