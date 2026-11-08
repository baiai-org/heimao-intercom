package net.openradiolink.app

import kotlinx.coroutines.*
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.TimeUnit

class BoardApiTest {
    @Test fun activityPttAndSetupRenewalBypassSlowSettingsWhileKeepingIdentityAndProxyPath()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/proxy/").toString()))
        api.deviceId="board-A"
        val slow=async(Dispatchers.IO){api.json("api/status")}
        try {
            server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE))
            assertEquals("/proxy/api/status",server.takeRequest(2,TimeUnit.SECONDS)!!.path)
            server.enqueue(MockResponse().setBody("""{"type":"activity"}"""))
            withTimeout(2000){assertEquals("activity",api.activity().getString("type"))}
            val activity=server.takeRequest(1,TimeUnit.SECONDS)!!
            assertEquals("/proxy/api/activity",activity.path)
            assertEquals("board-A",activity.getHeader("X-ORL-Device"))
            server.enqueue(MockResponse().setBody("""{"ok":true,"sid":10,"tx":2,"id":3,"ptt":false}"""))
            withTimeout(2000){api.sdrPtt(10,2,false,3,true)}
            assertEquals("/proxy/api/sdr_ptt?sid=10&tx=2&on=0&id=3&activity=1",server.takeRequest(1,TimeUnit.SECONDS)!!.path)
            server.enqueue(MockResponse().setBody("""{"ok":true,"timeout_ms":60000}"""))
            withTimeout(2000){api.setupSession("android-profile-A")}
            val hold=server.takeRequest(1,TimeUnit.SECONDS)!!
            assertEquals("/proxy/api/wifi/setup_session",hold.path)
            assertEquals("POST",hold.method);assertEquals("board-A",hold.getHeader("X-ORL-Device"))
            assertNull(hold.getHeader("X-ORL-Control"))
            assertEquals("renew",JSONObject(hold.body.readUtf8()).getString("op"))
            assertEquals(4,server.requestCount)
        } finally {slow.cancelAndJoin();api.close();server.shutdown()}
    }
    @Test fun pttRejectionRetainsCorrelatedReasonWithoutWeakeningOtherJsonRequests()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/proxy/").toString()))
        try {
            server.enqueue(MockResponse().setBody("""{"ok":false,"sid":10,"tx":2,"id":3,"ptt":false,"reason":"network_not_ready"}"""))
            val reply=api.sdrPtt(10,2,true,3)
            assertEquals("network_not_ready",reply.getString("reason"))
            assertEquals("/proxy/api/sdr_ptt?sid=10&tx=2&on=1&id=3",server.takeRequest().path)
            assertThrows(PttRejected::class.java){RadioStateRules.validatePtt(reply,10,2,3)}
            server.enqueue(MockResponse().setBody("""{"ok":false,"err":"rejected"}"""))
            try{api.json("api/set",JSONObject());fail("expected rejection")}
            catch(e:ApiFailure){assertEquals("rejected",e.message)}
            assertEquals(2,server.requestCount)
        } finally {api.close();server.shutdown()}
    }
    @Test fun jsonMutationKeepsSpecialCharactersAndIdentityHeaders()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/proxy/board/").toString(),bearer="test-entry"))
        try {
            api.deviceId="board-A";api.token="a".repeat(64)
            server.enqueue(MockResponse().setBody("{\"ok\":true}"))
            val value="%20 + & = 测试"
            api.json("api/set",JSONObject().put("k","example").put("v",value))
            val request=server.takeRequest(2,TimeUnit.SECONDS)!!
            assertEquals("/proxy/board/api/set",request.path)
            assertEquals("POST",request.method)
            assertEquals("board-A",request.getHeader("X-ORL-Device"))
            assertEquals("a".repeat(64),request.getHeader("X-ORL-Control"))
            assertEquals("Bearer test-entry",request.getHeader("Authorization"))
            assertEquals(value,JSONObject(request.body.readUtf8()).getString("v"))
            assertFalse(request.path!!.contains("test-entry"))
        } finally {api.close();server.shutdown()}
    }
    @Test fun rejectedOrRedirectedMutationIsNeverReplayed()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/").toString()))
        try {
            server.enqueue(MockResponse().setResponseCode(307).setHeader("Location",server.url("/other")).setBody("{}"))
            try{api.json("api/set",JSONObject().put("k","example").put("v","value"));fail("expected redirect rejection")}
            catch(e:ApiFailure){assertEquals(307,e.status)}
            assertEquals(1,server.requestCount)
            server.enqueue(MockResponse().setResponseCode(423).setBody("{\"ok\":false,\"err\":\"lease lost\"}"))
            try{api.json("api/set",JSONObject());fail("expected ownership failure")}
            catch(e:ApiFailure){assertEquals(423,e.status)}
            assertEquals(2,server.requestCount)
        } finally {api.close();server.shutdown()}
    }
    @Test fun controlClaimNamesThePhoneAndReportsAContendedLease()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/").toString()))
        try {
            api.deviceId="board-A"
            val token="b".repeat(64)
            server.enqueue(MockResponse().setBody("{\"ok\":true,\"held\":true,\"mine\":true,\"device_id\":\"board-A\",\"label\":\"黑猫对讲\",\"generation\":7,\"expires_in_ms\":15000,\"token\":\"$token\"}"))
            val granted=api.claim("android-1",false)
            assertEquals(7,granted.getInt("generation"))
            val claim=server.takeRequest(2,TimeUnit.SECONDS)!!
            assertEquals("/api/control",claim.path)
            assertEquals("POST",claim.method)
            assertEquals("board-A",claim.getHeader("X-ORL-Device"))
            assertNull(claim.getHeader("X-ORL-Control"))
            val body=JSONObject(claim.body.readUtf8())
            assertEquals("claim",body.getString("op"))
            assertEquals("android-1",body.getString("client"))
            assertEquals(Brand.APP,body.getString("label"))
            assertEquals("board-A",body.getString("device_id"))
            server.enqueue(MockResponse().setResponseCode(409).setBody("""{"ok":false,"err":"control occupied","held":true,"mine":false,"label":"黑猫控制面板"}"""))
            try{api.claim("android-1",true);fail("expected contention")}
            catch(e:ApiFailure){assertEquals(409,e.status);assertEquals("control occupied",e.message)}
            val takeover=server.takeRequest(2,TimeUnit.SECONDS)!!
            assertEquals("takeover",JSONObject(takeover.body.readUtf8()).getString("op"))
            assertEquals(2,server.requestCount)
        } finally {api.close();server.shutdown()}
    }
    @Test fun cancellationClosesOldRequestBeforeNextSession()=runBlocking {
        val server=MockWebServer();server.start()
        val api=BoardApi(AccessPoint(address=server.url("/").toString()))
        try {
            server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE))
            val pending=launch(Dispatchers.IO){api.json("api/status")}
            assertNotNull(server.takeRequest(2,TimeUnit.SECONDS))
            withTimeout(2000){pending.cancelAndJoin()}
            api.close()
            assertEquals(1,server.requestCount)
        } finally {api.close();server.shutdown()}
    }
}
