package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RadioActivityStateTest {
    private fun frame(boot:String="1234abcd",stamp:Long=100,tx:Boolean=false,rx:Boolean=false)=JSONObject("""{"type":"activity","device_id":"board-A","boot_id":"$boot","sample_us":$stamp,"tx_active":$tx,"rx_active":$rx,"ic_mode":1,"net_ready":true}""")
    private fun sample(boot:String="1234abcd",stamp:Long=100,tx:Boolean=false,rx:Boolean=false)=RadioActivitySample.parse(frame(boot,stamp,tx,rx),"board-A")!!
    @Test fun lateHttpCannotResurrectTxAfterNewerReleaseAck() {
        val transmitting=RadioActivityState().accept(sample(stamp=100,tx=true),10)
        val released=transmitting.accept(sample(stamp=200),20)
        assertSame(released,released.accept(sample(stamp=150,tx=true),30))
        val staleStatus=JSONObject("""{"ptt":"TX","fp":{"rf_tx":1,"net_rx":1}}""")
        assertEquals(RadioActivity.IDLE,RadioStateRules.activity(true,false,staleStatus,released.sample))
        assertEquals(RadioActivity.TRANSMIT,RadioStateRules.activity(true,true,staleStatus,released.sample))
    }
    @Test fun newBootAcceptsResetClockButLateOldBootIsDiscarded() {
        val original=RadioActivityState().accept(sample(stamp=9000000,tx=true),1)
        val reboot=original.accept(sample(boot="aabbccdd",stamp=10),2)
        assertEquals("aabbccdd",reboot.sample!!.bootId)
        assertSame(reboot,reboot.accept(sample(stamp=9900000,tx=true),3))
        assertSame(reboot,reboot.accept(sample(boot="aabbccdd",stamp=10,tx=true),4))
    }
    @Test fun snapshotRequiresCorrectBoardAndCompleteTypedFields() {
        assertNull(RadioActivitySample.parse(frame(),"board-B"))
        assertNull(RadioActivitySample.parse(frame(stamp=0),"board-A"))
        assertNull(RadioActivitySample.parse(frame(boot="bad"),"board-A"))
        assertNull(RadioActivitySample.parse(frame().put("tx_active","false"),"board-A"))
        assertNull(RadioActivitySample.parse(frame().apply{remove("rx_active")},"board-A"))
    }
    @Test fun realtimeReceiveAndIdleOverrideSlowStatusWithoutAffectingTxPriority() {
        assertEquals(RadioActivity.RECEIVE,RadioStateRules.activity(true,false,JSONObject(),sample(rx=true)))
        assertEquals(RadioActivity.TRANSMIT,RadioStateRules.activity(true,false,JSONObject(),sample(tx=true,rx=true)))
        assertEquals(RadioActivity.IDLE,RadioStateRules.activity(false,true,JSONObject(),sample(tx=true)))
        val timeline=RadioActivityState().accept(sample(),100)
        val live=timeline.live(JSONObject("""{"ic_mode":2,"net_ready":false,"rssi":50}"""))
        assertEquals(1,live.getInt("ic_mode"));assertTrue(live.getBoolean("net_ready"));assertEquals(50,live.getInt("rssi"))
        assertTrue(timeline.fresh(1600));assertFalse(timeline.fresh(1601))
    }
}
