package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DeviceMaintenanceTest {
    private fun state()=JSONObject().put("version",1).put("ready",true).put("revision",4)
        .put("device_id","orl-example").put("boot_id","boot-1").put("cleanup_enabled",true)
        .put("restart_enabled",false).put("restart_hours",24).put("busy","idle")
    @Test fun defaultsAndDurableFields() {
        val s=DeviceMaintenanceState.parse(state());assertTrue(s.available&&s.ready&&s.cleanup)
        assertFalse(s.restart);assertEquals(24,s.hours);assertEquals(4L,s.revision)
    }
    @Test fun absentOldFirmwareDoesNotEnableActions() {
        assertFalse(DeviceMaintenanceState.parse(JSONObject()).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("available",false)).ready)
        assertFalse(DeviceMaintenanceState.parse(state().put("ready",false)).ready)
    }
    @Test fun malformedConfigurationIsUnavailable() {
        for(hours in listOf(0,169))assertFalse(DeviceMaintenanceState.parse(state().put("restart_hours",hours)).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("cleanup_enabled","true")).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("revision",-1)).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("boot_id","")).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("restart_hours",24.5)).available)
        assertFalse(DeviceMaintenanceState.parse(state().put("revision",1.5)).available)
    }
    @Test fun pendingAndBusyAreNotReportedAsExecuted() {
        val s=DeviceMaintenanceState.parse(state().put("restart_enabled",true).put("restart_waiting",true)
            .put("reboot_pending",true).put("busy","receiving").put("cleanup_runs",3).put("released_buffer_bytes",0))
        assertTrue(s.pending&&s.waiting);assertEquals("receiving",s.busy);assertEquals(0L,s.bytes)
    }
    @Test fun labelsUseStableIdsAndLongUnicodeNames() {
        val json=JSONObject().put("np2p",JSONObject().put("labels_version",1).put("labels_ready",true).put("label_revision",7)
            .put("peers",org.json.JSONArray().put(JSONObject().put("id","m-001122334455").put("name","山顶中继").put("custom_label",true))))
        val np=Np2pState.parse(json);assertTrue(np.labelsReady);assertEquals(7L,np.labelRevision)
        assertEquals("m-001122334455",np.peers.single().id);assertEquals("山顶中继",np.peers.single().name)
        assertTrue(np.peers.single().customLabel)
    }
    @Test fun parsesWeeklyScheduleAndClock() {
        val data=state().put("version",2).put("restart_enabled",true).put("restart_days",21)
            .put("restart_minute",270).put("timezone","Asia/Shanghai").put("clock_state","synced")
            .put("last_sync_utc",1795075200L).put("next_restart_utc",1795120200L)
        val parsed=DeviceMaintenanceState.parse(data)
        assertTrue(parsed.available)
        assertEquals(2,parsed.apiVersion)
        assertEquals(21,parsed.days)
        assertEquals(270,parsed.minute)
        assertEquals("synced",parsed.clock)
        assertEquals(1795120200L,parsed.nextRestart)
    }

    @Test fun rejectsMalformedWeeklySchedule() {
        val data=state().put("version",2).put("restart_enabled",true).put("restart_days",0)
            .put("restart_minute",240).put("timezone","Asia/Shanghai")
        assertFalse(DeviceMaintenanceState.parse(data).available)
        assertFalse(DeviceMaintenanceState.parse(data.put("restart_days",128)).available)
        assertFalse(DeviceMaintenanceState.parse(data.put("restart_days",1).put("restart_minute",1440)).available)
        assertFalse(DeviceMaintenanceState.parse(data.put("restart_minute",1.5)).available)
        assertTrue(DeviceMaintenanceState.parse(data.put("restart_minute",1439)).available)
    }
}
