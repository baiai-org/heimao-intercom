package net.openradiolink.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ReleaseConfirmTest {
    @Test fun httpOkIsEnough() {
        assertTrue(ReleaseConfirm.closed(httpOk = true, error = null, boardOn = true))
    }

    @Test fun boardOffWinsOverHttpFailure() {
        assertTrue(ReleaseConfirm.closed(httpOk = false, error = "timeout", boardOn = false))
    }

    @Test fun staleSessionCountsAsClosed() {
        assertTrue(ReleaseConfirm.closed(httpOk = false, error = "audio session expired", boardOn = true))
    }

    @Test fun transmittingAndUnreadableIsNotClosed() {
        assertFalse(ReleaseConfirm.closed(httpOk = false, error = "audio session transmitting", boardOn = true))
        assertFalse(ReleaseConfirm.closed(httpOk = false, error = null, boardOn = null))
    }

    @Test fun forceStartsOnSecondAttempt() {
        assertFalse(ReleaseConfirm.forceOnAttempt(0))
        assertTrue(ReleaseConfirm.forceOnAttempt(1))
        assertTrue(ReleaseConfirm.forceOnAttempt(3))
    }
}
