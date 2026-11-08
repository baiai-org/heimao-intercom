package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class HandsetNameTest {
    @Test fun unicodeLimitsAndNormalization() {
        assertEquals("车载主台",HandsetName.normalized("  车载主台  "))
        assertEquals("é",HandsetName.normalized("e\u0301"))
        val emoji="\uD83D\uDC31".repeat(24)
        assertEquals(emoji,HandsetName.normalized(emoji))
        listOf("名".repeat(25),"\uD83D\uDC31".repeat(25),"x\u0000y","x\ny","x\u0085y","\uD800").forEach {
            assertThrows(IllegalArgumentException::class.java){HandsetName.normalized(it)}
        }
    }
    @Test fun legacyAliasAndSharedNameSurviveRoundTripAndMerge() {
        val legacy=JSONObject().put("id","profile").put("name","旧手机备注").put("deviceId","orl-board")
            .put("endpoints",org.json.JSONArray().put(AccessPoint(id="a",address="http://10.0.0.1/").json()))
        val old=DeviceProfile.from(legacy)
        assertEquals(-1L,old.nameRevision);assertEquals("旧手机备注",old.displayName)
        val shared=DeviceProfile.from(old.copy(customName="车载主台",nameRevision=5).json())
        assertEquals("车载主台",shared.displayName);assertEquals("旧手机备注",shared.name)
        val request=DeviceProfile(id="duplicate",name="新入口",deviceId="orl-board",endpoints=listOf(AccessPoint(id="b",address="http://10.0.0.2/")))
        val result=mergeProfiles(listOf(shared),request,"orl-board")
        assertEquals(1,result.first.size);assertEquals("车载主台",result.second.displayName)
        assertEquals("旧手机备注",result.second.copy(customName="").displayName)
    }
    @Test fun untrustedDiscoveryNameFallsBackToStableSuffix() {
        val id="orl-0123456789ab"
        assertEquals(Brand.discoveryName(id),HandsetName.discovery("bad\u0000name",id))
        assertEquals("外出手台",HandsetName.discovery("外出手台",id))
    }
}
