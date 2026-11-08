package net.openradiolink.app

import org.junit.Assert.*
import org.junit.Test

class KeepAliveRulesTest {
    @Test fun persistentSwitchControlsServiceRestart() {
        assertEquals(KeepAliveRules.Restart.STICKY,KeepAliveRules.restartMode(true))
        assertEquals(KeepAliveRules.Restart.NOT_STICKY,KeepAliveRules.restartMode(false))
    }
    @Test fun unknownBatteryStateIsNotReportedAsAllowed() {
        assertEquals("已允许",KeepAliveRules.batteryLabel(true))
        assertEquals("未允许",KeepAliveRules.batteryLabel(false))
        assertEquals("无法读取",KeepAliveRules.batteryLabel(null))
        assertNotEquals("已允许",KeepAliveRules.batteryLabel(null))
    }
    @Test fun manualConfirmationsAreExplicit() {
        assertEquals("已手动确认",KeepAliveRules.confirmLabel(true))
        assertEquals("未确认",KeepAliveRules.confirmLabel(false))
        assertEquals("常驻前台已开启",KeepAliveRules.persistentLabel(true))
    }
}
