package net.openradiolink.app

import org.junit.Assert.*
import org.junit.Test

class EqPresetsTest {
    @Test fun presetsMatchTheWebPanelCurves() {
        assertEquals(listOf(-4,-4,0,3,2,2),EqPresets.presets.first{it.id=="voice"}.values)
        assertEquals(listOf(-8,-8,0,5,5,3),EqPresets.presets.first{it.id=="punch"}.values)
        assertEquals(listOf(2,3,2,0,-3,-4),EqPresets.presets.first{it.id=="warm"}.values)
        assertEquals(List(6){0},EqPresets.presets.first{it.id=="flat"}.values)
        assertEquals(4,EqPresets.presets.size)
    }
    @Test fun bandsKeepTheFirmwareKeysAndOrder() {
        assertEquals(listOf("bass","warm","body","clear","pres","air"),EqPresets.bands.map{it.key})
        assertEquals(6,EqPresets.bands.size)
    }
    @Test fun valuesStayInsideTheFirmwareRange() {
        assertEquals(-12,EqPresets.clamp(-40))
        assertEquals(6,EqPresets.clamp(20))
        assertEquals(3,EqPresets.clamp(3))
        assertTrue(EqPresets.valid(listOf(0,-12,6,1,2,3)))
        assertFalse(EqPresets.valid(listOf(0,0,0,0,0,7)))
        assertFalse(EqPresets.valid(listOf(0,0,0,0,0)))
    }
    @Test fun flatIsTheOnlyAllZeroPreset() {
        val flat=EqPresets.active(List(6){0})
        assertNotNull(flat)
        assertEquals("flat",flat!!.id)
        assertNull(EqPresets.active(listOf(-4,-2,3,5,3,1)))
        assertEquals("+6",EqPresets.label(6))
        assertEquals("0",EqPresets.label(0))
        assertEquals("-12",EqPresets.label(-12))
    }
}
