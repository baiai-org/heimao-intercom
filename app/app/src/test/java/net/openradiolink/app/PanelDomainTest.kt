package net.openradiolink.app

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
}
