package net.openradiolink.app

/** One user-initiated setup request. Reconnection never creates another profile
 * or changes the verified handset, and a superseded Network cannot remove its replacement. */
class SetupRecovery<N>(private val timeoutMs: Long=60_000) {
    var generation=0L; private set
    var profile: DeviceProfile?=null; private set
    var network: N?=null; private set
    private var lostAt: Long?=null
    val active get()=profile!=null
    fun start(profile: DeviceProfile) { stop();this.profile=profile }
    fun available(value: N): DeviceProfile? {
        val current=profile ?: return null
        if(network==value)return null
        network=value;lostAt=null
        return current
    }
    fun lost(value: N,now: Long): Boolean {
        if(!active || network!=value)return false
        network=null;lostAt=now
        return true
    }
    fun bind(value: DeviceProfile) {
        val current=profile ?: return
        require(current.deviceId.isBlank() || current.deviceId==value.deviceId) { "配网连接返回了另一块手台" }
        profile=value
    }
    fun expired(now: Long)=active && lostAt?.let { now-it>=timeoutMs }==true
    fun stop() { generation++;profile=null;network=null;lostAt=null }
}
