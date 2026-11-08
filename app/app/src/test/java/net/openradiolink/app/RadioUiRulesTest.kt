package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RadioUiRulesTest {
    @Test fun frequencyAcceptsExactStepsAndBandEdges() {
        assertEquals("145.0125",RadioUiRules.frequency("145.0125","134","174"))
        assertEquals("400.0000",RadioUiRules.frequency("400","400","480"))
        assertEquals("480.0000",RadioUiRules.frequency("480","400","480"))
        listOf("399.9875","480.0125","433.1234","NaN","").forEach{input->assertThrows(Exception::class.java){RadioUiRules.frequency(input,"400","480")}}
        assertThrows(Exception::class.java){RadioUiRules.frequency("145","","")}
    }
    @Test fun unknownFrequencyDoesNotLookLikeARealTuning() {
        assertEquals("—.————",RadioUiRules.displayFrequency("0"))
        assertEquals("—.————",RadioUiRules.displayFrequency(""))
        assertEquals("145.5000",RadioUiRules.displayFrequency("145.5"))
    }
    @Test fun channelNamesUseFirmwareByteLimit() {
        assertNull(RadioUiRules.nameError("测试信道12345678901"))
        assertNotNull(RadioUiRules.nameError("测试信道123456789012"))
        assertNotNull(RadioUiRules.nameError("   "))
    }
    @Test fun favoritesSurviveIndexChangesAndDistinguishTones() {
        val c=JSONObject().put("i",4).put("name","本地").put("rx",433.5).put("tx",433.5).put("txt","0000")
        val key=RadioUiRules.channelKey(c)
        assertEquals(key,RadioUiRules.channelKey(JSONObject(c.toString()).put("i",1)))
        assertNotEquals(key,RadioUiRules.channelKey(JSONObject(c.toString()).put("txt","0067")))
    }
    @Test fun numericValidationUsesFirmwareLimits() {
        assertNull(RadioUiRules.parameterError("gain","255"))
        assertNotNull(RadioUiRules.parameterError("gain","0"))
        assertNull(RadioUiRules.parameterError("micgain","7"))
        assertNotNull(RadioUiRules.parameterError("sq","9"))
        assertNotNull(RadioUiRules.parameterError("eq_mic_air","6.1"))
    }
}
