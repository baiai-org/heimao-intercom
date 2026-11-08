package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class RadioStateRulesTest {
    private fun live(mode:Int,ready:Boolean)=JSONObject().put("ic_mode",mode).put("net_ready",ready)
    @Test fun p2pDenialExplainsModeAndUnconnectedBoards() {
        val text=RadioStateRules.networkBlock(live(1,false),JSONObject())!!
        assertTrue(text.contains("公网直连（P2P）"));assertTrue(text.contains("尚未连接到其他手台"));assertTrue(text.contains("禁止发射"))
        assertNull(RadioStateRules.networkBlock(live(1,true),JSONObject()))
    }
    @Test fun relayRequiresServiceReadinessRatherThanPeerCount() {
        val status=JSONObject("""{"intercom":{"mode":2,"peers":0}}""")
        assertNull(RadioStateRules.networkBlock(live(2,true),status))
        assertTrue(RadioStateRules.networkBlock(live(2,false),status)!!.contains("中继服务尚未连接"))
        assertNull(RadioStateRules.networkBlock(live(0,false),status))
        assertNull(RadioStateRules.networkBlock(JSONObject(),status)) // unknown readiness is not a denial
    }
    @Test fun rejectedPttIsInterpretedOnlyForMatchingRequestIds() {
        val reply=JSONObject("""{"ok":false,"sid":10,"tx":4,"id":3,"reason":"network_not_ready"}""")
        val rejection=assertThrows(PttRejected::class.java){RadioStateRules.validatePtt(reply,10,4,3)}
        assertEquals("network_not_ready",rejection.reason)
        val stale=assertThrows(IOException::class.java){RadioStateRules.validatePtt(reply,10,5,3)}
        assertFalse(stale is PttRejected)
        assertTrue(RadioStateRules.rejection(rejection.reason,1).contains("尚未连接到其他手台"))
        assertFalse(RadioStateRules.rejection(rejection.reason,1,true).contains("尚未连接到其他手台"))
    }
    @Test fun successfulAndCancelledRepliesKeepCorrelationChecks() {
        val ok=JSONObject("""{"ok":true,"sid":10,"tx":4,"id":3,"ptt":true}""")
        assertSame(ok,RadioStateRules.validatePtt(ok,10,4,3))
        assertThrows(IOException::class.java){RadioStateRules.validatePtt(ok,11,4,3)}
        assertFalse(RadioStateRules.rejection("busy",1).contains("未连接到其他手台"))
    }
    @Test fun confirmedTransmitWinsOverReceiveAndDisconnectClearsActivity() {
        val receive=JSONObject("""{"ptt":"IDLE","fp":{"rf_rx":1,"net_rx":1}}""")
        assertEquals(RadioActivity.RECEIVE,RadioStateRules.activity(true,false,receive))
        assertEquals(RadioActivity.TRANSMIT,RadioStateRules.activity(true,true,receive))
        assertEquals(RadioActivity.IDLE,RadioStateRules.activity(false,true,receive))
        assertEquals(RadioActivity.TRANSMIT,RadioStateRules.activity(true,false,JSONObject("""{"ptt":"TX(BRIDGE)"}""")))
        assertEquals(RadioActivity.TRANSMIT,RadioStateRules.activity(true,false,JSONObject("""{"fp":{"net_tx":1,"net_rx":1}}""")))
    }
    @Test fun networkReceptionIsActivityButListeningAndMetersAreNot() {
        assertEquals(RadioActivity.RECEIVE,RadioStateRules.activity(true,false,JSONObject("""{"fp":{"net_rx":1}}""")))
        assertEquals(RadioActivity.RECEIVE,RadioStateRules.activity(true,false,JSONObject("""{"ptt_net":{"rx_carrier":1}}""")))
        val idle=JSONObject("""{"ptt":"PREPARE","peak":32767,"rssi":99,"sql_open":true,"sdr":{"on":true},"intercom":{"peers":3}}""")
        assertEquals(RadioActivity.IDLE,RadioStateRules.activity(true,false,idle))
        assertEquals(RadioActivity.IDLE,RadioStateRules.activity(true,false,JSONObject("""{"ptt":"TAIL","fp":{"rf_rx":0}}""")))
    }
}
