package net.openradiolink.app

import okhttp3.HttpUrl.Companion.toHttpUrl
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.Base64

class ReleaseUpdateManifestTest {
    private val fixture=javaClass.getResourceAsStream("/release-update/update.json")!!.use{it.readBytes()}
    private val packageName="net.openradiolink.app"

    @Test fun signedCatalogKeepsComponentModesIndependent() {
        val candidate=ReleaseUpdateManifest.decode(fixture,packageName,199)!!
        assertFalse(candidate.force)
        assertEquals("fixture-200",candidate.version)
        assertEquals("https://github.com/baiai-org/heimao-intercom/releases/download/app-200-test/mock.apk",candidate.url.toString())
        assertNull(ReleaseUpdateManifest.decode(fixture,packageName,200))
        assertNull(ReleaseUpdateManifest.decode(fixture,"different.package",1))
    }

    @Test fun tamperingWithForceFlagBreaksSignature() {
        val envelope=JSONObject(fixture.decodeToString())
        val payload=JSONObject(Base64.getDecoder().decode(envelope.getString("payload")).decodeToString())
        payload.getJSONObject("app").put("force",true)
        envelope.put("payload",Base64.getEncoder().encodeToString(payload.toString().encodeToByteArray()))
        assertThrows(IllegalArgumentException::class.java){
            ReleaseUpdateManifest.decode(envelope.toString().encodeToByteArray(),packageName,1)
        }
    }

    @Test fun redirectsStayInTheOfficialReleaseDownloadEndpoints() {
        assertTrue(ReleaseUpdateManifest.allowed(ReleaseUpdateManifest.latest))
        assertTrue(ReleaseUpdateManifest.allowed("https://release-assets.githubusercontent.com/asset".toHttpUrl()))
        for(url in listOf("http://github.com/baiai-org/heimao-intercom/releases/x",
            "https://github.com/other/heimao-intercom/releases/x",
            "https://github.com/baiai-org/heimao-intercom/releases-other/x",
            "https://release-assets.githubusercontent.com.example.com/file",
            "https://evil@github.com/baiai-org/heimao-intercom/releases/x")) {
            assertFalse(ReleaseUpdateManifest.allowed(url.toHttpUrl()))
        }
    }
}
