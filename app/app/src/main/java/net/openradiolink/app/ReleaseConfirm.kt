package net.openradiolink.app

/**
 * Decides whether the board really stopped listening.
 *
 * A single HTTP reply is not enough: the board can answer `ok:false` for a
 * stale session, a request can time out while the engine is already off, and
 * the tail/release path used to refuse closes.  The board's own `/api/status`
 * is the authority; anything else is only a shortcut.
 */
object ReleaseConfirm {
    /** Retry once normally, then ask the board to force the close. */
    fun forceOnAttempt(attempt: Int): Boolean = attempt >= 1

    fun closed(httpOk: Boolean, error: String?, boardOn: Boolean?): Boolean {
        if (httpOk) return true
        if (boardOn == false) return true
        return error?.contains("expired") == true
    }
}
