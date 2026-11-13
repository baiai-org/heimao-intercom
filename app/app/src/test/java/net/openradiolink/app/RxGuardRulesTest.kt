package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RxGuardRulesTest {
    @Test fun millisecondPrecisionWithoutFloatRounding() {
        listOf("0.001" to 1,"0.037" to 37,"1.234" to 1234,"60.000" to 60000).forEach{(s,ms)->
            assertEquals(ms,RxGuardRules.delay(s,false))
            assertEquals(s,RxGuardRules.format(ms,false))
            assertEquals(ms,RxGuardRules.delay(ms.toString(),true))
        }
    }
    @Test fun refusesTruncationOverflowAndMalformedTimes() {
        listOf("0","0.0001","60.001","-1","+1","1e2","NaN","","1.",".1","9999999999999999999999").forEach{s->
            assertThrows(Exception::class.java){RxGuardRules.delay(s,false)}
        }
        listOf("1.5","0","60001","1e3").forEach{s->assertThrows(Exception::class.java){RxGuardRules.delay(s,true)}}
    }
    @Test fun atomicConfigAndMasks() {
        assertEquals("1,1234,5,3",RxGuardRules.encode(RxGuardRules.Config(true,1234,5,3)))
        listOf(0,8,256).forEach{mask->
            assertThrows(Exception::class.java){RxGuardRules.encode(RxGuardRules.Config(true,1,mask,7))}
            assertThrows(Exception::class.java){RxGuardRules.encode(RxGuardRules.Config(true,1,7,mask))}
        }
        assertEquals("rx_guard_p2p",RxGuardRules.key(1));assertEquals("rx_guard_relay",RxGuardRules.key(2))
        assertThrows(Exception::class.java){RxGuardRules.key(0)}
    }
    @Test fun oldFirmwareAndIndependentModeReadback() {
        assertNull(RxGuardRules.read(JSONObject(),1))
        val s=JSONObject("""{"rx_guard":{"version":1,"p2p":{"en":1,"ms":37,"sources":1,"outputs":7},"relay":{"en":0,"ms":60000,"sources":6,"outputs":1}}}""")
        assertEquals(RxGuardRules.Config(true,37,1,7),RxGuardRules.read(s,1))
        assertEquals(RxGuardRules.Config(false,60000,6,1),RxGuardRules.read(s,2))
        s.getJSONObject("rx_guard").put("version",2);assertNull(RxGuardRules.read(s,1))
    }
}
