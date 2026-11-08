package net.openradiolink.app

import android.content.Context
import android.net.Network
import android.os.SystemClock
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

class BoardSession(
    private val context: Context,
    initialProfile: DeviceProfile,
    network: Network?,
    private val update: (BoardSession,SessionView)->Unit,
    private val bind: suspend (DeviceProfile,String)->DeviceProfile,
    private val cache: suspend (DeviceProfile,JSONObject,Long)->Unit,
    private val microphoneType: (Boolean)->Unit,
    private val playbackType: (Boolean)->Unit,
    private val canRecord: ()->Boolean,
    private val trust: (BoardSession,CertificateFailure)->Unit,
    private val nameCache: suspend (DeviceProfile)->Unit = {},
):OwnedSession {
    var profile=initialProfile;private set
    val api=BoardApi(profile.endpoint(),network)
    val view=MutableStateFlow(SessionView(profileId=profile.id,phase="核对设备身份"))
    private val scope=CoroutineScope(SupervisorJob()+Dispatchers.IO)
    private val client="android-"+UUID.randomUUID().toString().replace("-","")
    private val audioLock=Mutex();private val pttLock=Mutex()
    private val actions=Mutex()
    private val nameLock=Mutex()
    private val press=AtomicLong();private val reads=AtomicLong()
    @Volatile private var closed=false
    @Volatile private var audio: PcmAudio?=null
    @Volatile private var listeningWanted=false
    @Volatile private var sid=0L
    @Volatile private var tx=0L
    @Volatile private var heldTx=0L
    @Volatile private var pttMayBeActive=false
    private var lastRenew=0L
    @Volatile private var leaseWanted=false
    @Volatile private var takeoverArmed=true
    private var takeoverTries=0
    private var capabilities=JSONObject()
    private var setupHold=false
    private val realtime get()=capabilities.optInt("radio_activity")>=1
    fun currentView():SessionView=if(closed)view.value.copy(connected=false,
        phase=if(view.value.phase.startsWith("等待原手台"))view.value.phase else "正在关闭原手台连接") else view.value
    private fun publish(change: (SessionView)->SessionView) {
        view.update(change)
        update(this,currentView())
    }
    private fun acceptActivity(json:JSONObject) {
        if(closed)return
        val sample=RadioActivitySample.parse(json,api.deviceId) ?: return
        val now=SystemClock.elapsedRealtime()
        publish { old -> old.copy(activity=old.activity.accept(sample,now)) }
    }
    private suspend fun readActivity() { acceptActivity(api.activity()) }
    private fun liveNow()=view.value.activity.live(view.value.live)
    fun perform(action: suspend BoardSession.()->Unit): Job=scope.launch {
        if (closed) return@launch
        try { actions.withLock { if (!closed) action() } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { report(e) }
    }
    private fun report(e: Throwable) {
        if (closed) return
        if (e is CertificateFailure) trust(this,e)
        if (e is ApiFailure && e.status==423) {
            api.token="";listeningWanted=false;cancelPress();takeoverArmed=true
            /* 423: another client holds the lease; the read-only notice below
             * is the one the operator should keep, so no extra error here. */
            scope.launch { runCatching { stopListening(announce=false) } }
            /* 423 means another client holds the lease: go read-only and let the
             * operator decide; nothing is re-acquired silently. */
            publish { it.copy(mine=false,error="控制权已被其它入口取得，已转为只读") }
        } else if(e is PttDenied)publish{it.copy(error=e.message ?: "发话被阻止",pttNotice=e.message ?: "发话被阻止",pttNoticeId=SystemClock.elapsedRealtimeNanos())}
        else publish { it.copy(error=e.message ?: "连接失败") }
    }
    override suspend fun connect() {
        leaseWanted=false;takeoverArmed=true;takeoverTries=0
        verifyIdentity()
        if(profile.endpoint().mode=="SETUP" && capabilities.optInt("setup_session")>=1) {
            renewSetup()
            scope.launch {
                while(isActive && !closed) {
                    delay(15_000)
                    try { renewSetup() }
                    catch(e:CancellationException){throw e}
                    catch(e:Exception){report(e)}
                }
            }
        }
        if(realtime)readActivity()
        refresh(true)
        // Control stays with whoever the operator chooses; no silent acquisition.
        if(realtime)scope.launch {
            while(isActive && !closed) {
                val started=SystemClock.elapsedRealtime()
                if(!view.value.maintenance) {
                    val wsAt=audio?.lastActivityAt ?: 0L
                    if(wsAt==0L || started-wsAt>1500) {
                        try { withTimeout(2000){readActivity()} }
                        catch(e:TimeoutCancellationException){ /* bounded, single-flight fallback */ }
                        catch(e:CancellationException){throw e}
                        catch(e:Exception){if(e is CertificateFailure)report(e)}
                    }
                }
                val period=if(canRecord())200L else 3000L
                delay((period-(SystemClock.elapsedRealtime()-started)).coerceAtLeast(20L))
            }
        }
        scope.launch {
            var lastFull=SystemClock.elapsedRealtime()
            while (isActive && !closed) {
                delay(if(canRecord())1000 else 3000)
                if (view.value.maintenance) continue
                try {
                    if (!view.value.connected) verifyIdentity()
                    if(SystemClock.elapsedRealtime()-lastFull>=3000 || !view.value.connected){refresh(false);lastFull=SystemClock.elapsedRealtime()}
                    else if(!realtime)refreshActivity()
                } catch (e: CancellationException) { throw e }
                catch (e: Exception) {
                    cancelPress();report(e)
                    publish { it.copy(connected=false,phase=if(e is IllegalArgumentException)"设备身份核对失败" else "等待当前设备恢复",ptt=false,pttPending=false) }
                    if(e is IllegalArgumentException) {
                        api.token="";runCatching { stopListening() };api.close();break
                    }
                }
            }
        }
        // One beat for the lease: renew while owned, otherwise keep trying to take
        // it back silently (a free board, or a web page that went idle).
        scope.launch {
            while (isActive && !closed) {
                delay(5000)
                if (view.value.maintenance) continue
                if (api.token.isNotBlank() && view.value.mine) {
                    try { applyControl(api.json("api/control",JSONObject().put("op","renew")));lastRenew=SystemClock.elapsedRealtime() }
                    catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        api.token="";listeningWanted=false;cancelPress();takeoverArmed=true
                        publish { it.copy(mine=false) }
                        scope.launch { runCatching { stopListening() } }
                        report(e)
                    }
                }
            }
        }
    }
    private suspend fun verifyIdentity() {
        val info=api.json("api/device")
        require(info.optInt("api_version")==2) { "手台需要升级到配套 Wi-Fi App 固件" }
        val id=info.optString("device_id")
        require(id.isNotBlank() && id.length<=64) { "设备身份无效" }
        require(profile.deviceId.isBlank() || profile.deviceId==id) { "这个地址返回了另一块手台，已停止连接" }
        require(api.deviceId.isBlank() || api.deviceId==id) { "设备身份发生变化，请核对访问入口" }
        api.deviceId=id;capabilities=info.optJSONObject("capabilities") ?: JSONObject()
        profile=bind(profile,id)
        if(capabilities.optInt("custom_name")>=1)acceptName(info,true)
        else if(profile.nameRevision>=0) { profile=profile.copy(customName="",nameRevision=-1);nameCache(profile) }
        publish { it.copy(profileId=profile.id,deviceId=id,phase="读取实际状态",error="") }
    }
    private suspend fun acceptName(data:JSONObject,verified:Boolean=false)=nameLock.withLock {
        if(closed || data.optString("device_id")!=api.deviceId || !data.has("custom_name") || !data.has("name_revision"))return@withLock
        val name=HandsetName.normalized(data.getString("custom_name"))
        val revision=data.getLong("name_revision")
        require(revision in 0..4294967295L) { "手台名称版本无效" }
        if(!verified && revision<profile.nameRevision)return@withLock
        if(profile.customName!=name || profile.nameRevision!=revision) {
            profile=profile.copy(customName=name,nameRevision=revision)
            nameCache(profile)
        }
    }
    suspend fun renameHandset(input:String,expected:Long) {
        val name=HandsetName.normalized(input)
        check(capabilities.optInt("custom_name")>=1) { "升级手台固件后可同步名称" }
        ensureLease();reads.incrementAndGet()
        try {
            val reply=api.json("api/device/name",JSONObject().put("custom_name",name).put("name_revision",expected))
            check(reply.optString("device_id")==api.deviceId && reply.getString("custom_name")==name) { "手台名称回读不一致" }
            acceptName(reply);refresh()
        } catch(e:ApiFailure) {
            if(e.status==409) { refresh();throw IOException("名称已变化或存储不可用，请核对最新名称后重试") }
            throw e
        }
    }
    private suspend fun renewSetup() {
        api.setupSession("android-${profile.id}")
        setupHold=true
    }
    private fun applyControl(control: JSONObject) {
        val wasOwned=false
        val mine=true   /* no lease: a connected session may always act */
        control.remove("token")
        publish { it.copy(control=control,mine=mine) }
        if (wasOwned && !mine) {
            api.token="";listeningWanted=false;cancelPress()
            scope.launch { runCatching { stopListening() } }
        }
    }
    suspend fun refresh(tables: Boolean=false) {
        val version=reads.get()
        val status=api.json("api/status")
        if(closed || version!=reads.get())return
        acceptName(status)
        // Publish the status now; configuration queries do not hold up activity.
        publish { it.copy(status=status,connected=true,phase="已连接",updated=System.currentTimeMillis(),error="") }
        val live=api.json("api/live")
        val radio=api.json("api/radio")
        val wifi=api.json("api/wifi")
        val control=api.json("api/control")
        val channels=if (tables) JSONArray(String(api.bytes("api/channels",256*1024),Charsets.UTF_8)) else view.value.channels
        if (closed || version!=reads.get()) return
        applyControl(control)
        publish { it.copy(status=status,live=live,radio=radio,wifi=wifi,channels=channels,connected=true,
            phase="已连接",updated=System.currentTimeMillis(),error="") }
    }
    private suspend fun refreshActivity() {
        val version=reads.get()
        val status=api.json("api/status")
        val live=api.json("api/live")
        if(closed || version!=reads.get())return
        acceptName(status)
        publish{it.copy(status=status,live=live,updated=System.currentTimeMillis())}
    }
    suspend fun claim(takeover: Boolean=false, hold: Boolean=true) {
        check(!closed && view.value.connected)
        leaseWanted=true
        val result=try {
            api.claim(client,takeover,hold)
        } catch (e: ApiFailure) {
            if (e.status==409 && !takeover) throw ControlHeld(ControlRules.holder(view.value.control))
            throw e
        }
        val token=result.optString("token");require(token.length==64) { "手台没有签发控制凭据" }
        api.token=token;lastRenew=SystemClock.elapsedRealtime();applyControl(result)
        takeoverArmed=false;takeoverTries=0
    }
    /* The board has no control lease any more: one operator uses it, and the lease
     * only produced refusals (a page left open, a phone that reconnected). */
    private suspend fun ensureLease() {
        check(!closed && view.value.connected) { "当前手台尚未连接" }
    }
    private suspend fun radioIdle(): Boolean = try { ControlRules.idle(api.json("api/status")) }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { false }
    suspend fun claimIfNeeded()=ensureLease()
    suspend fun release() {
        leaseWanted=false;takeoverArmed=false
        stopListening()
        if (api.token.isNotBlank()) api.json("api/control",JSONObject().put("op","release"))
        api.token="";publish { it.copy(mine=false) };refresh()
    }
    suspend fun set(key: String,value: String) {
        ensureLease();reads.incrementAndGet()
        check(capabilities.optBoolean("json_settings")) { "需要支持新版参数接口的配套固件" }
        api.json("api/set",JSONObject().put("k",key).put("v",value));refresh()
    }
    /**
     * One user action, six bands. The firmware's /api/set accepts a single band per
     * request, so this keeps the lock and the capability check once and refreshes
     * after the last write instead of after every band.
     */
    suspend fun setEq(direction: String,values: List<Int>) {
        require(direction=="spk" || direction=="mic") { "EQ 方向必须是 spk 或 mic" }
        require(EqPresets.valid(values)) { "EQ 需要 6 段，范围 −12～+6 dB" }
        ensureLease();reads.incrementAndGet()
        check(capabilities.optBoolean("json_settings")) { "需要支持新版参数接口的配套固件" }
        values.forEachIndexed { i,value ->
            api.json("api/set",JSONObject().put("k","eq_${direction}_${EqPresets.bands[i].key}").put("v",value.toString()))
        }
        refresh()
    }
    /** Firmware resets both directions at once, exactly like the web panel's "全部归 0 dB". */
    suspend fun resetEq() {
        ensureLease();reads.incrementAndGet()
        check(capabilities.optBoolean("json_settings")) { "需要支持新版参数接口的配套固件" }
        api.json("api/set",JSONObject().put("k","eq_reset").put("v","0"));refresh()
    }
    suspend fun channel(op: String,index: Int?=null,name: String="") {
        ensureLease();reads.incrementAndGet()
        val url=EndpointAddress.api(api.base,"api/channel").newBuilder().addQueryParameter("op",op)
        index?.let { url.addQueryParameter("i",it.toString()) }
        if (op=="save" || op=="edit") url.addQueryParameter("name",name)
        api.json("api/channel?"+url.build().encodedQuery);refresh(true)
    }
    suspend fun wifi(body: JSONObject):JSONObject {
        ensureLease();reads.incrementAndGet();val result=api.json("api/wifi",body);refresh();return result
    }
    suspend fun startListening()=audioLock.withLock {
        check(!closed)
        if (audio!=null) return@withLock
        ensureLease();listeningWanted=true
        val opened=api.json("api/sdr?on=1&client=${(SystemClock.elapsedRealtime()%4294967294L)+1}")
        sid=opened.getLong("sid");tx=opened.optLong("tx")
        val pipe=PcmAudio(context,api,sid,realtime,::acceptActivity,
            onLevel={ level -> if(!closed)publish{it.copy(microphoneLevel=level)} }) { problem ->
            if (!closed) {
                cancelPress();listeningWanted=false
                scope.launch { runCatching { stopListening() };report(problem) }
            }
        }
        audio=pipe
        try {
            playbackType(true);pipe.open()
            if (closed) throw CancellationException("device closed")
            publish { it.copy(listening=true,pttReason=pipe.transport,error="") }
        } catch (e: Exception) {
            withContext(NonCancellable) {
                pipe.close();audio=null;playbackType(false)
                runCatching { withTimeout(2000) { api.json("api/sdr?on=0&sid=$sid") } };sid=0
            }
            throw e
        }
    }
    /* Closes the board session and reports whether the close reached the
     * board.  A !ok reply from the board (已失效 / not enabled / close
     * timeout) already means the engine is off; the only real failure is a
     * close that never got there -- the one case that can leave a dead
     * listener behind -- and it is surfaced instead of swallowed. */
    suspend fun stopListening(announce: Boolean = true): Boolean {
        listeningWanted=false;stopPtt()
        return audioLock.withLock {
            val old=sid
            audio?.close();audio=null;playbackType(false)
            /* Nothing was open: the engine is already off. */
            var confirmed = old==0L
            var detail=""
            if (old!=0L) {
                val deadline=SystemClock.elapsedRealtime()+5000L
                var attempt=0
                while (!confirmed && SystemClock.elapsedRealtime()<deadline) {
                    var httpOk=false
                    var errorText:String?=null
                    try {
                        api.json("api/sdr?on=0&sid=$old" +
                            if (ReleaseConfirm.forceOnAttempt(attempt)) "&force=1" else "")
                        httpOk=true
                    } catch (cancel: CancellationException) { throw cancel }
                    catch (e: Exception) {
                        errorText=e.message ?: e.javaClass.simpleName
                        detail="HTTP ${(e as? ApiFailure)?.status ?: "-"} $errorText"
                    }
                    /* The board's state is the authority; a lost reply or a
                     * close refused during the tail must not fail the upload
                     * when the engine is already down. */
                    val boardOn=if (httpOk) false else sdrOn()
                    confirmed=ReleaseConfirm.closed(httpOk,errorText,boardOn)
                    if (!confirmed && boardOn!=false) {
                        detail=if (detail.isEmpty()) "板端仍报告 sdr.on=true" else detail
                        ++attempt
                        delay(250)
                    }
                }
            }
            sid=0
            publish { s -> s.copy(listening=false,ptt=false,pttPending=false,
                error=if(announce && old!=0L && !confirmed && !closed)
                    "手台仍未停止收听（$detail）；请先松键，再重试上传"
                else s.error) }
            confirmed
        }
    }
    private suspend fun sdrOn():Boolean? = try {
        api.json("api/status").optJSONObject("sdr")?.optBoolean("on")
    } catch (cancel: CancellationException) { throw cancel }
    catch (_: Exception) { null }
    fun beginPress() {
        if (closed || !canRecord()) return
        /* One over at a time: a rapid second press while the previous OFF is
         * still in flight used to advance tx and then send an OFF the board
         * had never accepted, which could leave the carrier keyed. */
        if (heldTx!=0L || pttMayBeActive) return
        val ticket=press.incrementAndGet()
        perform {
            var t=0L
            var pipe:PcmAudio?=null
            var ackId=0L
            var currentLive:JSONObject?=null
            var attemptMode:Int?=null
            try {
                pttLock.withLock {
                    check(audio!=null && view.value.listening) { "请先开启收听" }
                    ensureLease()
                    if (ticket!=press.get() || !canRecord()) return@withLock
                    val live=if(realtime) {
                        if(!view.value.activity.fresh(SystemClock.elapsedRealtime()) &&
                            withTimeoutOrNull(2000){readActivity();true}!=true)
                            throw IOException("未能及时获取手台状态，请检查连接后重试")
                        liveNow()
                    } else api.json("api/live")
                    if(ticket!=press.get() || !canRecord())return@withLock
                    currentLive=live;attemptMode=RadioStateRules.mode(live,view.value.status)
                    publish{it.copy(live=live)}
                    RadioStateRules.networkBlock(live,view.value.status)?.let{throw PttDenied(it)}
                    val next=tx+1;check(next<=4294967295L) { "请重新开启通联会话" };tx=next;t=tx
                    heldTx=t
                    pttMayBeActive=true
                    publish { it.copy(pttPending=true,error="") }
                    pipe=audio
                    microphoneType(true)
                    if (pipe!!.transport=="WebSocket") {
                        /* Fire the ON frame, start capture at once, and only
                         * wait for its ACK outside this lock: a blocked ACK
                         * must never delay the operator's release. */
                        ackId=pipe.sendPtt(true,t)
                        if (ackId==0L) throw IOException("PTT 指令发送失败")
                        pipe.startMicrophone(t)
                    } else {
                        val reply=pipe.ptt(true,t)
                        check(reply.optBoolean("ptt")) { "手台尚未进入发话状态" }
                        pipe.startMicrophone(t)
                    }
                }
                if (ackId!=0L) {
                    val reply=pipe!!.awaitPtt(ackId,sid,t)
                    if (ticket!=press.get() || !canRecord()) throw CancellationException("press released")
                    check(reply.optBoolean("ptt")) { "手台尚未进入发话状态" }
                    publish { it.copy(ptt=true,pttPending=false,pttReason="正在发话",audioSid=sid,audioTx=t) }
                } else if (t!=0L) {
                    publish { it.copy(ptt=true,pttPending=false,pttReason="正在发话",audioSid=sid,audioTx=t) }
                }
            } catch (e: Exception) {
                audio?.stopMicrophone();microphoneType(false)
                runCatching { withTimeout(3000) { api.json("api/sdr_ptt?on=0&sid=$sid&tx=$t&id=0") } }
                publish { it.copy(ptt=false,pttPending=false) }
                if(e is PttRejected && (e.reason=="busy"||e.reason=="owner_lost")) {
                    /* The board may still hold the previous PTT owner.  Try
                     * an OFF with the token the board actually reports, then
                     * a forced close if the owner is still stuck. */
                    val stuck=runCatching{api.json("api/sdr_state?sid=$sid")}.getOrNull()
                    val boardTx=stuck?.optLong("txid")?:0L
                    if(boardTx>0L) {
                        runCatching{withTimeout(1500){api.json("api/sdr_ptt?on=0&sid=$sid&tx=$boardTx&id=0")}}
                    }
                    val owner=stuck?.optString("owner").orEmpty()
                    if(owner.isNotBlank() && owner!="none") {
                        runCatching{withTimeout(2000){api.json("api/sdr?on=0&sid=$sid&force=1")}}
                    }
                }
                if(e is PttRejected) {
                    var latest=currentLive ?: JSONObject()
                    if(e.reason=="network_not_ready") {
                        try {
                            if(realtime)withTimeoutOrNull(1000){readActivity();latest=liveNow()}
                            else withTimeoutOrNull(1000){api.json("api/live")}?.let{latest=it;publish{v->v.copy(live=it)}}
                        }
                        catch(cancel:CancellationException){throw cancel}
                        catch(_:Exception){ }
                    }
                    val ready=latest.optBoolean("net_ready").takeIf{latest.has("net_ready")}
                    throw PttDenied(RadioStateRules.rejection(e.reason,RadioStateRules.mode(latest,view.value.status) ?: attemptMode,ready),e)
                }
                if (e !is CancellationException) throw e
            }
        }
    }
    fun cancelPress() {
        press.incrementAndGet()
        if (!closed) scope.launch { runCatching { stopPtt() }.onFailure { report(it) } }
    }
    suspend fun stopPtt()=pttLock.withLock {
        val wasHeld=pttMayBeActive || view.value.ptt || view.value.pttPending
        /* Send the 880/660 tone first: stopMicrophone() clears `recording`, so
         * the old order silently skipped the tone on every release. */
        audio?.finishMicrophone();audio?.stopMicrophone();microphoneType(false)
        if (wasHeld && sid!=0L && tx!=0L) {
            val s=sid; val t=if(heldTx!=0L) heldTx else tx
            var confirmed=false
            var detail=""
            runCatching {
                // Try the already-open WS first; HTTP below is the authority.
                val pipe=audio
                if(pipe?.transport=="WebSocket") {
                    val reply=withTimeoutOrNull(800){runCatching{pipe.ptt(false,t)}.getOrNull()}
                    if(reply!=null && !reply.optBoolean("ptt"))confirmed=true
                }
            }
            val deadline=SystemClock.elapsedRealtime()+2500L
            var attempt=0
            while(!confirmed && SystemClock.elapsedRealtime()<deadline) {
                runCatching {
                    val suffix=if(realtime)"&activity=1" else ""
                    val released=api.json("api/sdr_ptt?on=0&sid=$s&tx=$t&id=0$suffix")
                    released.optJSONObject("activity")?.let(::acceptActivity)
                    if(!released.optBoolean("ptt"))confirmed=true
                }.onFailure { e ->
                    detail="HTTP ${(e as? ApiFailure)?.status ?: "-"} ${e.message ?: e.javaClass.simpleName}"
                }
                if(!confirmed) {
                    val state=runCatching{api.json("api/sdr_state?sid=$s${if(realtime)"&activity=1" else ""}")}.getOrNull()
                    state?.optJSONObject("activity")?.let(::acceptActivity)
                    if(state?.optBoolean("ptt")==false)confirmed=true
                }
                ++attempt
                if(!confirmed && attempt%4==0) {
                    /* 800 ms without an accepted OFF: only force when the board
                     * still reports an actual PTT hold.  During tail (rel) the
                     * OFF was accepted; forcing there would cut the courtesy
                     * tone and recreate "carrier but no tail". */
                    val state=runCatching{api.json("api/sdr_state?sid=$s${if(realtime)"&activity=1" else ""}")}.getOrNull()
                    val holding=state?.optBoolean("ptt")==true
                    val releasing=state?.optBoolean("rel")==true || state?.optBoolean("tail")==true
                    val owner=state?.optString("owner").orEmpty()
                    val stuckOwner=owner.isNotBlank() && owner!="none" && !holding && !releasing
                    if((holding && !releasing) || stuckOwner) {
                        val forced=runCatching{api.json("api/sdr?on=0&sid=$s&force=1")}.isSuccess
                        if(forced){confirmed=true;audio?.close();audio=null;playbackType(false);sid=0}
                    }
                }
                if(!confirmed)delay(200)
            }
            if(!confirmed && !closed) {
                publish { it.copy(error=if(detail.isEmpty())"未能确认发射已停止" else "未能确认发射已停止（$detail）") }
            }
            pttMayBeActive=false
        }
        heldTx=0L
        publish { it.copy(ptt=false,pttPending=false,listening=audio!=null,pttReason=audio?.transport ?: "") }
    }
    suspend fun upload(data: ByteArray,firmware: Boolean) {
        ensureLease()
        if (!stopListening()) throw IOException("收听未能在手台侧确认关闭，无法上传")
        reads.incrementAndGet()
        publish { it.copy(maintenance=true,progress=0,phase=if(firmware)"上传固件" else "上传尾音",error="") }
        try {
            api.json("api/control",JSONObject().put("op","renew"));lastRenew=SystemClock.elapsedRealtime()
            var result=JSONObject()
            try { result=api.upload(if(firmware)"api/ota" else "api/tail_upload",data) { progress -> publish { it.copy(progress=progress) } } }
            catch(e: IOException) { if(e is ApiFailure || !firmware || view.value.progress<100)throw e }
            if (firmware) {
                api.token="";publish { it.copy(phase="等待重启并核对完整镜像",mine=false) }
                var verified=false
                repeat(40) {
                    if (!verified) {
                        delay(2000)
                        try {
                            verifyIdentity()
                            val running=api.bytes("api/firmware.bin")
                            verified=BoardApi.sha256(running)==BoardApi.sha256(data)
                        } catch (_: IOException) { }
                    }
                }
                check(verified) { "未能确认新固件运行，请核对设备后再试" }
            }
            publish { it.copy(phase=if(firmware)"升级校验完成" else "尾音上传完成",progress=100,
                taskResult=if(firmware)"完整运行镜像与所选文件一致。SHA256：${BoardApi.sha256(data)}" else "尾音已转换保存，长度 ${result.optDouble("seconds")} 秒") }
        } finally {
            publish { it.copy(maintenance=false) }
            runCatching { refresh(true) }
        }
    }
    suspend fun exportFirmware(write: suspend (ByteArray)->Unit) {
        ensureLease();stopListening()
        publish { it.copy(maintenance=true,progress=-1,phase="下载运行固件",taskResult="") }
        try {
            val data=api.bytes("api/firmware.bin");write(data)
            publish { it.copy(taskResult="固件已保存。SHA256：${BoardApi.sha256(data)}") }
        } finally { publish { it.copy(maintenance=false,phase="已连接") } }
    }
    suspend fun diagnostic(op: String) {
        ensureLease();stopListening()
        api.json("api/speaker_diag?op=$op&ms=${if(op=="arm"||op=="replay")5000 else 3000}",JSONObject())
    }
    override suspend fun close() {
        if (closed) return
        closed=true;press.incrementAndGet();audio?.stopMicrophone()
        scope.cancel()
        withContext(NonCancellable) {
            try {
                stopPtt();stopListening()
                if (api.token.isNotBlank()) runCatching { withTimeout(2000) { api.json("api/control",JSONObject().put("op","release")) } }
                if (setupHold) runCatching { withTimeout(1000) {
                    api.setupSession("android-${profile.id}",release=true)
                } }
            } finally {
                api.token=""
                // TLS close_notify can write to the socket; selection is initiated on Main.
                withContext(Dispatchers.IO) { api.close() }
            }
            scope.coroutineContext[Job]?.join()
            if (view.value.updated>0) runCatching { cache(profile,view.value.status,view.value.updated) }
        }
    }
}
