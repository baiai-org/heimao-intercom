package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class ControlRulesTest {
    private fun control(held: Boolean,mine: Boolean=false,label: String=""):JSONObject =
        JSONObject().put("held",held).put("mine",mine).apply { if(label.isNotEmpty()) put("label",label) }
    private fun status(ptt: String="IDLE",sdr: Boolean=false,keyed: Boolean=false,rx: Boolean=false):JSONObject =
        JSONObject().put("ptt",ptt).put("sdr",JSONObject().put("on",sdr))
            .put("bridge",JSONObject().put("keyed",keyed).put("rx_open",rx))

    @Test fun claimsAFreeLeaseWithoutAsking() {
        assertEquals(ControlAction.CLAIM,ControlRules.action("",control(false),status(),true))
        assertEquals(ControlAction.CLAIM,ControlRules.action("",control(false),null,false))
    }

    @Test fun keepsTheLeaseItAlreadyHolds() {
        assertEquals(ControlAction.NONE,ControlRules.action("t".repeat(64),control(true,true,Brand.APP),status(),true))
    }

    @Test fun replacesAnIdleWebPanel() {
        assertEquals(ControlAction.TAKEOVER,ControlRules.action("",control(true,false,Brand.PANEL),status(),true))
    }

    @Test fun leavesAnActiveWebPanelAlone() {
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.PANEL),status(ptt="TX"),true))
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.PANEL),status(sdr=true),true))
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.PANEL),status(keyed=true),true))
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.PANEL),status(rx=true),true))
    }

    @Test fun neverFightsAnotherPhone() {
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.APP),status(),true))
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,"另一个手机"),status(),true))
    }

    @Test fun honoursReleaseAndAFinishedTakeoverAttempt() {
        assertEquals(ControlAction.NONE,ControlRules.action("",control(true,false,Brand.PANEL),status(),false))
    }

    @Test fun namesTheCurrentHolder() {
        assertEquals(Brand.PANEL,ControlRules.holder(control(true,false,Brand.PANEL)))
        assertEquals("android-1",ControlRules.holder(JSONObject().put("held",true).put("client","android-1")))
        assertEquals("其他客户端",ControlRules.holder(JSONObject().put("held",true)))
    }
}
