package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class NetworkModesTest {
    @Test fun threeCardsInFirmwareOrder() {
        assertEquals(listOf(0,1,2),NetworkModes.cards.map{it.mode})
        assertEquals("本地电台",NetworkModes.cards[0].title)
        assertEquals("公网直连 P2P",NetworkModes.cards[1].title)
        assertEquals("网络中继",NetworkModes.cards[2].title)
    }
    @Test fun onlyTheActiveModeIsCurrent() {
        assertTrue(NetworkModes.isCurrent(1,1))
        assertFalse(NetworkModes.isCurrent(2,1))
    }
    @Test fun inactiveModesReadAsNotEnabled() {
        val (label,linked)=NetworkModes.status(1,2,"idle",3)
        assertEquals("未启用 · 点按切换",label)
        assertFalse(linked)
    }
    @Test fun localModeIsAlwaysDirect() {
        val (label,linked)=NetworkModes.status(0,0,"idle",0)
        assertEquals("本机直通",label)
        assertTrue(linked)
    }
    @Test fun p2pNeedsALinkedPeer() {
        val waiting=NetworkModes.status(1,1,"idle",0)
        assertEquals("待机",waiting.first)
        assertFalse(waiting.second)
        val linked=NetworkModes.status(1,1,"idle",2)
        assertEquals("待机 · 2 在线用户",linked.first)
        assertTrue(linked.second)
        val noNet=NetworkModes.status(1,1,"no-net",0)
        assertEquals("无网络",noNet.first)
        assertFalse(noNet.second)
    }
    @Test fun mqttConnectionIgnoresPeerCountsAndRecentTraffic() {
        val intercom=JSONObject().put("mode",2).put("state","rx").put("peers",5)
        val status=JSONObject().put("intercom",intercom)
        val live=JSONObject().put("ic_mode",2).put("net_ready",true)
        val disconnected=NetworkModes.relayStatus(status,live,true)
        assertEquals(NetworkModes.RelayTone.NEUTRAL,disconnected.tone)
        intercom.put("relay_link","connected")
        val linked=NetworkModes.relayStatus(status,live,true)
        assertEquals(NetworkModes.RelayTone.CONNECTED,linked.tone)
        assertEquals("✓ 中继已连接",linked.title)
        intercom.put("peers",0)
        assertEquals(linked,NetworkModes.relayStatus(status,live,true))
        intercom.put("relay_link","disconnected")
        assertEquals(NetworkModes.RelayTone.DOWN,NetworkModes.relayStatus(status,live,true).tone)
        assertEquals(NetworkModes.RelayTone.NEUTRAL,NetworkModes.relayStatus(status,live,false).tone)
    }
    @Test fun relayAuthenticationFailureAndRetryHaveExplicitText() {
        val intercom=JSONObject().put("mode",2).put("relay_link","error").put("relay_reason","authentication")
        val status=JSONObject().put("intercom",intercom)
        val failed=NetworkModes.relayStatus(status,JSONObject(),true)
        assertEquals(NetworkModes.RelayTone.DOWN,failed.tone)
        assertTrue(failed.reason.contains("用户名"))
        intercom.put("relay_link","reconnecting")
        assertEquals(NetworkModes.RelayTone.WAITING,NetworkModes.relayStatus(status,JSONObject(),true).tone)
    }
    @Test fun missingEngineNeverReadsAsLinked() {
        for(state in listOf("no-engine","stub","off")) {
            val intercom=JSONObject().put("mode",2).put("state",state)
            assertEquals(NetworkModes.RelayTone.DOWN,
                NetworkModes.relayStatus(JSONObject().put("intercom",intercom),JSONObject(),true).tone)
        }
        assertFalse(NetworkModes.status(1,1,"no-engine",0).second)
    }
    @Test fun unknownStateIsShownInsteadOfBlank() {
        assertEquals("未知状态",NetworkModes.stateLabel(""))
        assertEquals("待机",NetworkModes.stateLabel("idle"))
        assertEquals("发话中",NetworkModes.stateLabel("tx"))
    }
}
