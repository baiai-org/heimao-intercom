package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class PanelDomainTest {
    @Test fun acceptsPlainAndDashedNames() {
        for(name in listOf("heimao","heimao-u13","a1","x-9-y")) assertTrue(name,PanelDomain.valid(name))
    }
    @Test fun rejectsWhatTheFirmwareRejects() {
        for(name in listOf("","-heimao","heimao-","he imao","heimao_u13","域名","heimao.","a".repeat(64))) {
            assertFalse(name,PanelDomain.valid(name))
        }
        assertTrue(PanelDomain.valid("a".repeat(63)))
    }
    @Test fun normalizesBeforeUse() {
        assertEquals("heimao-u13",PanelDomain.normalize("  HeiMao-U13 "))
        assertEquals("https://heimao-u13.local",PanelDomain.address("heimao-u13"))
        assertEquals("",PanelDomain.address(""))
    }
    @Test fun explainsTheFirstProblem() {
        assertEquals("请填写管理面板访问域名",PanelDomain.error(""))
        assertEquals("首字符必须是字母或数字",PanelDomain.error("-ab"))
        assertEquals("末字符必须是字母或数字",PanelDomain.error("ab-"))
        assertEquals("只能用字母、数字和连字符",PanelDomain.error("ab_c"))
    }

    @Test fun cachedIpRequiresMatchingLanProfileAndVerifiedIdentity() {
        val wifi=JSONObject().put("connected",true).put("host","heimao").put("mdns_host","heimao-2")
            .put("ip","192.168.0.12")
        val status=JSONObject().put("device_id","board-a").put("wifi",wifi)
        val endpoint=AccessPoint(address="https://heimao-2.local/",certificate="pin")
        fun profile(entry:AccessPoint=endpoint)=DeviceProfile(name="QA",deviceId="board-a",endpoints=listOf(entry),
            lastStatus=status.toString())
        assertEquals("192.168.0.12",PanelDomain.cachedIp(profile())?.hostAddress)
        assertNull(PanelDomain.cachedIp(profile().copy(deviceId="board-b")))
        assertNull(PanelDomain.cachedIp(profile(endpoint.copy(mode="SETUP"))))
        assertNull(PanelDomain.cachedIp(profile(endpoint.copy(address="https://other.local/"))))
        assertNull(PanelDomain.cachedIp(profile(endpoint.copy(address="http://heimao.local/",bearer="local-test"))))
        wifi.put("ip","127.0.0.1")
        assertNull(PanelDomain.cachedIp(profile()))
    }
}
