package net.openradiolink.app

import org.json.JSONObject

/** What the phone should do about the board control lease, without asking the user. */
enum class ControlAction { NONE, CLAIM, TAKEOVER }

object ControlRules {
    /** Handing the board over is safe only when nothing transmits, listens or bridges. */
    fun idle(status: JSONObject): Boolean {
        val sdr=status.optJSONObject("sdr")
        val bridge=status.optJSONObject("bridge")
        return status.optString("ptt")=="IDLE" &&
            sdr?.optBoolean("on")!=true &&
            bridge?.optBoolean("keyed")!=true &&
            bridge?.optBoolean("rx_open")!=true
    }

    /**
     * Claim a free lease and replace an idle web panel. Two phones never take over
     * from each other: that would leave both of them read-only.
     */
    fun action(token: String,control: JSONObject,status: JSONObject?,takeoverArmed: Boolean): ControlAction {
        if (control.optBoolean("mine") && token.isNotBlank()) return ControlAction.NONE
        if (!control.optBoolean("held")) return ControlAction.CLAIM
        if (control.optString("label")!=Brand.PANEL) return ControlAction.NONE
        if (!takeoverArmed || status==null || !idle(status)) return ControlAction.NONE
        return ControlAction.TAKEOVER
    }

    fun holder(control: JSONObject): String =
        control.optString("label").takeIf { it.isNotBlank() }
            ?: control.optString("client").takeIf { it.isNotBlank() }
            ?: "其他客户端"
}
