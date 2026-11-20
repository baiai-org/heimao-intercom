package net.openradiolink.app

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class EqUserPresetsTest {
    private val flat=EqCurves(List(6){0},List(6){0})
    private val tuned=EqCurves(listOf(1,2,3,4,5,6),listOf(-1,-2,-3,-4,-5,-6))
    private val empty=EqSlots("orl-a","1234abcd",0,List(3){null},flat)

    @Test fun savedDraftDoesNotModifyTheActiveEq() {
        val save=EqSlotWrite(EqSlotOp.SAVE,2,empty,"夜间",tuned)
        val next=empty.copy(revision=1,slots=listOf(null,EqUserSlot("夜间",tuned),null))
        assertEquals(2,save.body().getInt("slot"))
        assertTrue(save.confirmed(next))
        assertFalse(save.confirmed(next.copy(boot="11111111")))
        assertFalse(save.confirmed(next.copy(revision=2)))
    }

    @Test fun applyChecksBothDirectionsAndDoesNotAlterTheSlots() {
        val base=empty.copy(revision=3,slots=listOf(EqUserSlot("车载",tuned),null,null))
        val apply=EqSlotWrite(EqSlotOp.APPLY,1,base)
        assertEquals(flat,EqCurves.parse(apply.body(),"expected_"))
        assertFalse(apply.confirmed(base.copy(active=tuned.copy(mic=flat.mic))))
        assertTrue(apply.confirmed(base.copy(active=tuned)))
    }

    @Test fun snapshotRequiresTheSameDeviceAndExactlyThreeSlots() {
        val json=flat.putInto(JSONObject(),"active_").put("device_id","orl-a")
            .put("boot_id","1234abcd").put("revision",0)
            .put("slots",JSONArray(listOf(JSONObject.NULL,JSONObject.NULL,JSONObject.NULL)))
        assertEquals(empty,EqSlots.parse(json,"orl-a"))
        assertThrows(IllegalArgumentException::class.java){EqSlots.parse(json,"orl-b")}
        json.put("slots",JSONArray())
        assertThrows(IllegalArgumentException::class.java){EqSlots.parse(json,"orl-a")}
        assertTrue(EqUserSlot.nameValid("🐈".repeat(24)))
        assertFalse(EqUserSlot.nameValid("🐈".repeat(25)))
        assertFalse(EqUserSlot.nameValid("bad\u0000name"))
    }
}
