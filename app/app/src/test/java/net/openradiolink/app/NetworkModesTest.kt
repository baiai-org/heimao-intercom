package net.openradiolink.app

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
    @Test fun relayIsLinkedOnlyWhenAnEngineIsConnected() {
        assertTrue(NetworkModes.status(2,2,"idle",5).second)
        assertEquals("待机 · 5 个在线用户",NetworkModes.status(2,2,"idle",5).first)
        assertTrue(NetworkModes.status(2,2,"tx",5).second)
        assertFalse(NetworkModes.status(2,2,"connecting",0).second)
        assertFalse(NetworkModes.status(2,2,"stopped",0).second)
        assertFalse(NetworkModes.status(2,2,"no-net",0).second)
    }
    @Test fun missingEngineNeverReadsAsLinked() {
        for(state in listOf("no-engine","stub","off")) {
            val (label,linked)=NetworkModes.status(2,2,state,0)
            assertEquals(NetworkModes.stateLabel(state),label)
            assertFalse(state,linked)
        }
        assertFalse(NetworkModes.status(1,1,"no-engine",0).second)
    }
    @Test fun unknownStateIsShownInsteadOfBlank() {
        assertEquals("未知状态",NetworkModes.stateLabel(""))
        assertEquals("待机",NetworkModes.stateLabel("idle"))
        assertEquals("发话中",NetworkModes.stateLabel("tx"))
    }
}
