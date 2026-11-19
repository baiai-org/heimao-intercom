package net.openradiolink.app

import org.json.JSONObject

internal data class TailSettings(
    val id: Int,
    val modes: Int,
    val mode: Int,
    val enabled: Boolean,
    val customSeconds: Double,
    val customPlayable: Boolean,
) {
    fun description(titles: List<String>): String = when {
        id == 0 -> "尾音已关闭"
        mode !in 0..2 -> "正在确认当前模式"
        modes and (1 shl mode) == 0 -> "当前模式未启用尾音"
        enabled -> "当前模式已启用 · ${titles[id]}"
        else -> "所选尾音不可播放"
    }

    companion object {
        fun parse(status: JSONObject): TailSettings? {
            val tail = status.optJSONObject("tail") ?: return null
            val id = tail.optInt("id", -1)
            val modes = tail.optInt("modes", 0)
            val seconds = tail.optDouble("custom_s")
            if (id !in 0..8 || modes !in 1..7 || !seconds.isFinite() || seconds < 0) return null
            val mode = tail.optInt("mode", status.optJSONObject("intercom")?.optInt("mode", -1) ?: -1)
            val playable = if (tail.has("custom_play_ms")) tail.optInt("custom_play_ms") > 0 else seconds > 0
            val enabled = tail.optBoolean("enabled",
                id != 0 && mode in 0..2 && modes and (1 shl mode) != 0 && (id != 8 || playable))
            return TailSettings(id, modes, mode, enabled, seconds, playable)
        }
    }
}
