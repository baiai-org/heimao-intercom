package net.openradiolink.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DeniedPeersTest {
    @Test fun policyCapabilityAndOldFirmware() {
        assertEquals(0,Np2pState.parse(JSONObject("""{"np2p":{}}""")).accessVersion)
        val state=Np2pState.parse(JSONObject("""{"np2p":{"access_version":1,"access_ready":false,"denied_count":64,"denied_limit":64}}"""))
        assertEquals(1,state.accessVersion);assertFalse(state.accessReady)
        assertEquals(64,state.deniedCount);assertEquals(64,state.deniedLimit)
    }
    @Test fun stableIdsAndIdentityScope() {
        val rows=parseDeniedPeers(JSONObject("""{"ready":true,"count":2,"entries":[
          {"id":"m-020304050607","name":"B","ip":"2001:db8::42","port":54321,"identity":true},
          {"id":"a-4-01020304-1234","name":"old","ip":"192.0.2.42","port":1234,"identity":false}]}"""))
        assertEquals("m-020304050607",rows[0].id);assertTrue(rows[0].identity)
        assertEquals("[2001:db8::42]:54321",rows[0].address);assertFalse(rows[1].identity)
    }
    @Test(expected=IllegalStateException::class) fun incompleteListCannotOfferWrongRow() {
        parseDeniedPeers(JSONObject("""{"ready":true,"count":2,"entries":[]}"""))
    }
    @Test(expected=IllegalStateException::class) fun unreadablePolicyCannotPretendEmpty() {
        parseDeniedPeers(JSONObject("""{"ready":false,"count":0,"entries":[]}"""))
    }
}
