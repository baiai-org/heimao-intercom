package net.openradiolink.app

import org.junit.Assert.*
import org.junit.Test

class SetupRecoveryTest {
    private fun profile()=DeviceProfile(name="手台",endpoints=listOf(AccessPoint(address="https://192.168.4.1/",mode="SETUP")))
    @Test fun replacementNetworkSurvivesDelayedLossOfPreviousNetwork() {
        val state=SetupRecovery<String>();val profile=profile();state.start(profile)
        assertEquals(profile,state.available("old"))
        assertEquals(profile,state.available("new"))
        assertFalse(state.lost("old",100));assertEquals("new",state.network)
        assertNull(state.available("new"));assertFalse(state.expired(100_000))
    }
    @Test fun reconnectRetainsVerifiedIdentityAndDraftsWithoutCreatingProfiles() {
        val state=SetupRecovery<String>();state.start(profile());state.available("old")
        val canonical=profile().copy(deviceId="handset-A",drafts=mapOf("ssid" to "home"))
        state.bind(canonical);assertTrue(state.lost("old",0))
        assertFalse(state.expired(59_999));assertEquals(canonical,state.available("new"))
        assertFalse(state.expired(60_000));assertEquals("home",state.profile?.drafts?.get("ssid"))
        assertThrows(IllegalArgumentException::class.java){state.bind(canonical.copy(deviceId="handset-B"))}
    }
    @Test fun lossAtBootZeroHasAnExactBoundAndCancellationInvalidatesCallbacks() {
        val state=SetupRecovery<String>();state.start(profile());val ticket=state.generation
        state.available("wifi");assertTrue(state.lost("wifi",0))
        assertFalse(state.expired(59_999));assertTrue(state.expired(60_000))
        state.stop();assertNotEquals(ticket,state.generation)
        assertNull(state.available("late"));assertFalse(state.lost("wifi",61_000))
        assertFalse(state.expired(90_000));assertNull(state.profile)
        state.start(profile());assertNotEquals(ticket,state.generation)
    }
}
