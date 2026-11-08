package net.openradiolink.app

import org.json.JSONObject

data class RadioActivitySample(
    val bootId:String, val sampleUs:Long, val transmit:Boolean, val receive:Boolean,
    val mode:Int, val networkReady:Boolean,
) {
    companion object {
        fun parse(json:JSONObject,deviceId:String):RadioActivitySample? {
            if(json.optString("type")!="activity" || deviceId.isBlank() || json.optString("device_id")!=deviceId)return null
            val boot=json.optString("boot_id")
            val stamp=json.optLong("sample_us",-1)
            val mode=json.optInt("ic_mode",-1)
            if(!boot.matches(Regex("[0-9a-fA-F]{8}")) || stamp<=0 || mode !in 0..2)return null
            if(listOf("tx_active","rx_active","net_ready").any{json.opt(it) !is Boolean})return null
            return RadioActivitySample(boot,stamp,json.getBoolean("tx_active"),json.getBoolean("rx_active"),mode,json.getBoolean("net_ready"))
        }
    }
}

/** Immutable ordering state: HTTP, WS and correlated PTT ACKs share one timeline.
 * A late response from a retired boot cannot reset the new ESP timer epoch. */
data class RadioActivityState(
    val sample:RadioActivitySample?=null,
    val receivedAt:Long=0,
    val retiredBoots:List<String> = emptyList(),
) {
    fun accept(next:RadioActivitySample,now:Long):RadioActivityState {
        val previous=sample ?: return copy(sample=next,receivedAt=now)
        if(next.bootId==previous.bootId) {
            if(next.sampleUs<=previous.sampleUs)return this
            return copy(sample=next,receivedAt=now)
        }
        if(next.bootId in retiredBoots)return this
        return copy(sample=next,receivedAt=now,retiredBoots=(retiredBoots+previous.bootId).takeLast(16))
    }
    fun live(fallback:JSONObject):JSONObject = sample?.let {
        JSONObject(fallback.toString()).put("ic_mode",it.mode).put("net_ready",it.networkReady)
    } ?: fallback
    fun fresh(now:Long)=sample!=null && now-receivedAt in 0..1500
}
