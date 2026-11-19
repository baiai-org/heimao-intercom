package net.openradiolink.app

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException
import java.util.concurrent.TimeUnit

class DenoiseSettingsTest {
    private fun state(mode:Int,levels:Int,device:String="board-A")=
        JSONObject().put("ok",true).put("device_id",device).put("mode",mode).put("levels",levels)

    @Test fun localPathsAreSharedAndNetworkPathsAreIndependent() {
        assertEquals(listOf(4,3),DenoiseSettings.paths(0).map{it.setting})
        assertEquals(listOf(4,5,3,1,6,7),DenoiseSettings.paths(1).map{it.setting})
        assertEquals(listOf(4,8,3,2,9,10),DenoiseSettings.paths(2).map{it.setting})
        assertEquals(DenoiseSettings.paths(0)[1],DenoiseSettings.paths(2)[2])
    }

    @Test fun editSendsOnlyTheFieldsInTheCurrentMode() {
        val p2p=DenoiseSettings.paths(1)
        val mqtt=DenoiseSettings.paths(2)
        val write=DenoiseWrite(1,mapOf(
            p2p[1] to DenoiseDraft(DenoiseLevel.LIGHT,DenoiseLevel.EXTREME),
            p2p[4] to DenoiseDraft(DenoiseLevel.STRONG,DenoiseLevel.OFF),
        ))
        val body=write.body()
        assertEquals(1,body.getInt("mode"))
        assertEquals((1 shl 5) or (1 shl 6),body.getInt("mask"))
        assertEquals((1 shl 12) or (3 shl 15),body.getInt("expected_levels"))
        assertEquals(4 shl 12,body.getInt("levels"))
        val unrelated=2 shl mqtt[1].shift
        assertTrue(write.confirmed(DenoiseSettings.parse(state(2,unrelated or (4 shl 12)),"board-A")))
        assertFalse(write.confirmed(DenoiseSettings.parse(state(1,2 shl 12),"board-A")))
        assertThrows(IllegalArgumentException::class.java) {
            DenoiseWrite(1,mapOf(mqtt[1] to DenoiseDraft(DenoiseLevel.OFF,DenoiseLevel.STRONG)))
        }
    }

    @Test fun acceptsAllFiveLevelsAndRejectsLegacyOrMalformedReplies() {
        for(level in DenoiseLevel.entries) {
            val settings=DenoiseSettings.parse(state(0,level.ordinal shl 9),"board-A")
            assertEquals(level,settings.level(settings.paths.first()))
        }
        assertThrows(DenoiseUnsupported::class.java) {
            val legacy=state(0,0)
            legacy.remove("levels")
            DenoiseSettings.parse(legacy,"board-A")
        }
        assertThrows(IOException::class.java){DenoiseSettings.parse(state(0,0,"board-B"),"board-A")}
        assertThrows(IOException::class.java){DenoiseSettings.parse(state(0,5 shl 27),"board-A")}
        assertThrows(IOException::class.java){DenoiseSettings.parse(state(3,0),"board-A")}
        assertThrows(IOException::class.java){DenoiseSettings.parse(state(0,0).put("levels",1.5),"board-A")}
    }

    @Test fun uncertainSaveReadsBackOnceWithoutResending()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/proxy/").toString()))
        api.deviceId="board-A"
        try {
            val path=DenoiseSettings.paths(0).first()
            val write=DenoiseWrite(0,mapOf(path to DenoiseDraft(DenoiseLevel.OFF,DenoiseLevel.STRONG)))
            server.enqueue(MockResponse().setResponseCode(503).setBody("""{"ok":false}"""))
            server.enqueue(MockResponse().setBody(state(0,3 shl path.shift).toString()))
            assertTrue(write.confirmed(api.writeDenoise(write)))
            val sent=server.takeRequest(1,TimeUnit.SECONDS)!!
            val read=server.takeRequest(1,TimeUnit.SECONDS)!!
            assertEquals("/proxy/api/rf_denoise",sent.path)
            assertEquals("POST",sent.method)
            assertEquals("board-A",sent.getHeader("X-ORL-Device"))
            assertEquals(1 shl 4,JSONObject(sent.body.readUtf8()).getInt("mask"))
            assertEquals(sent.path,read.path)
            assertEquals("GET",read.method)
            assertEquals(2,server.requestCount)
        } finally {api.close();server.shutdown()}
    }

    @Test fun aConflictReturnsTheNewStoredLevelWithoutConfirmingTheDraft()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/").toString()))
        api.deviceId="board-A"
        try {
            val path=DenoiseSettings.paths(2).last()
            val write=DenoiseWrite(2,mapOf(path to DenoiseDraft(DenoiseLevel.OFF,DenoiseLevel.EXTREME)))
            server.enqueue(MockResponse().setResponseCode(409).setBody("""{"ok":false}"""))
            server.enqueue(MockResponse().setBody(state(2,2 shl path.shift).toString()))
            val current=api.writeDenoise(write)
            assertEquals(DenoiseLevel.STANDARD,current.level(path))
            assertFalse(write.confirmed(current))
            assertEquals(2,server.requestCount)
        } finally {api.close();server.shutdown()}
    }
}
