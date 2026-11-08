package net.openradiolink.app

import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.Network
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import androidx.core.content.ContextCompat
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicLong

class OrlApp:Application() {
    val manager by lazy { DeviceManager(this) }
    override fun onCreate() { super.onCreate();manager.scope.launch { manager.store.load() } }
}

class DeviceManager(private val context: Context) {
    val scope=CoroutineScope(SupervisorJob()+Dispatchers.Main.immediate+CoroutineExceptionHandler { _,error ->
        if(error !is CancellationException)message(error.message ?: "操作未完成，请重试")
    })
    val store=DeviceStore(context)
    val view=MutableStateFlow(SessionView())
    val certificates=MutableStateFlow<CertificateQuestion?>(null)
    val discovered=MutableStateFlow<List<DiscoveredBoard>>(emptyList())
    val messages=MutableSharedFlow<String>(extraBufferCapacity=16)
    private val slot=SingleDeviceSlot<BoardSession>()
    private var switching: Job?=null
    private var desired=""
    private val intentEpoch=AtomicLong()
    private var retry: Job?=null
    @Volatile var visible=false;private set
    @Volatile var service: SessionService?=null
    private val provision=ProvisionNetwork(context)
    private val setup=SetupRecovery<Network>()
    private var setupTimeout: Job?=null
    private var discoveryForList=false
    private var awaiting: DeviceProfile?=null
    private val discovery=DeviceDiscovery(context,::onDiscovered) { messages.tryEmit(it) }

    fun message(text: String) { messages.tryEmit(text) }
    fun setVisible(value: Boolean) {
        visible=value
        if (!value) { slot.current?.cancelPress();if(awaiting==null){discovery.stop();discoveryForList=false} }
    }
    private fun keepService() {
        if (service==null) ContextCompat.startForegroundService(context,Intent(context,SessionService::class.java))
    }
    /** 常驻前台开关：没有会话时也让服务与通知存活，系统回收后更愿意重启它。 */
    fun setPersistentForeground(value: Boolean) {
        KeepAlive.setPersistent(context,value)
        if (value) keepService()
        else if (view.value.profileId.isBlank() && slot.current==null) context.stopService(Intent(context,SessionService::class.java))
        service?.display()
    }
    /** Sticky service restarted after the process was killed: reconnect the last handset once. */
    fun restoreAfterRestart() {
        if (!KeepAlive.persistent(context)) return
        if (desired.isNotBlank() || awaiting!=null || slot.current!=null || view.value.profileId.isNotBlank()) return
        scope.launch {
            if (!store.loaded.value) store.loaded.first { it }
            val id=KeepAlive.lastProfile(context)
            if (id.isNotBlank() && desired.isBlank() && slot.current==null && view.value.profileId.isBlank() &&
                store.profiles.value.any { it.id==id }) connect(id)
        }
    }
    fun connect(id: String,endpointId: String?=null) {
        val profile=store.profiles.value.firstOrNull { it.id==id } ?: return
        val selected=if(endpointId==null)profile else profile.copy(selectedEndpoint=endpointId)
        if(selected.endpoint().mode=="SETUP") {
            if(setup.active && setup.profile?.id==selected.id && setup.network!=null) {
                setup.bind(selected);connectProfile(selected,setup.network)
            } else provisionBoard(selected)
        } else connectProfile(selected)
    }
    private fun connectProfile(profile: DeviceProfile,network: Network?=null) {
        if (busy()) { message("请等待当前设备的文件操作完成后再切换");return }
        if(profile.endpoint().mode!="SETUP") { setup.stop();setupTimeout?.cancel() }
        val epoch=intentEpoch.incrementAndGet()
        awaiting=null;desired=profile.id;KeepAlive.setLastProfile(context,profile.id);switching?.cancel();retry?.cancel();keepService()
        switching=scope.launch {
            val old=view.value
            view.value=old.copy(connected=false,phase=if(old.profileId.isBlank())"连接 ${profile.displayName}" else "正在释放原手台并切换至 ${profile.displayName}",error="")
            var attempted: BoardSession?=null
            try {
                slot.select {
                    if(profile.endpoint().mode=="SETUP" && (!setup.active || setup.network!=network || network==null))
                        throw CancellationException("配网网络已被替换")
                    if(network==null && profile.endpoint().mode!="SETUP")provision.close()
                    BoardSession(context,profile,network,
                        update={session,_ -> scope.launch { if(slot.isCurrent(session)){
                            val state=session.currentView()
                            if(state.error.isNotBlank() && state.error!=view.value.error)message(state.error)
                            view.value=state;service?.display()
                        } } },
                        bind=::bindIdentity,
                        cache={device,status,seen -> store.edit(device.id){it.copy(lastSeen=seen,lastStatus=status.toString())} },
                        microphoneType={ enabled -> service?.microphone(enabled) ?: if(enabled)throw IllegalStateException("连接服务尚未准备好") else Unit },
                        playbackType={enabled -> service?.playback(enabled)},
                        canRecord={visible},
                        trust={session,error -> scope.launch { if(slot.isCurrent(session))askCertificate(session.profile,error) } },
                        nameCache={ device -> store.edit(device.id){it.copy(customName=device.customName,nameRevision=device.nameRevision)} }
                    ).also { attempted=it }
                }
                slot.current?.let { desired=it.profile.id }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                if(BuildConfig.DEBUG)android.util.Log.w("ORL-Session","Connection failed",e)
                if(e is CertificateFailure)askCertificate(attempted?.profile ?: profile,e)
                view.value=view.value.copy(profileId=attempted?.profile?.id ?: profile.id,connected=false,mine=false,listening=false,
                    phase=if(e is CertificateFailure)"等待证书确认" else "连接失败",error=e.message ?: "无法连接")
                service?.display()
                if(e is java.io.IOException && e !is CertificateFailure && e !is ApiFailure) {
                    retry=scope.launch {
                        delay(4000)
                        if(epoch==intentEpoch.get() && desired==(attempted?.profile?.id ?: profile.id)) {
                            if(profile.endpoint().mode=="SETUP") {
                                // Always build a new API/client on the latest live Network.
                                val target=setup.profile
                                val live=setup.network
                                if(target!=null && live!=null)connectProfile(target,live)
                            } else connectProfile(profile)
                        }
                    }
                }
            }
        }
    }
    private suspend fun bindIdentity(requested: DeviceProfile,id: String): DeviceProfile {
        lateinit var canonical: DeviceProfile
        store.update { all ->
            val result=mergeProfiles(all,requested,id);canonical=result.second;result.first
        }
        withContext(Dispatchers.Main.immediate) {
            if(requested.endpoint().mode=="SETUP" && setup.profile?.id==requested.id) {
                setup.bind(canonical);desired=canonical.id
            }
        }
        return canonical
    }
    private fun askCertificate(profile: DeviceProfile,error: CertificateFailure) {
        certificates.value=CertificateQuestion(profile.id,profile.endpoint().id,error.host,error.fingerprint,error.audio)
    }
    fun acceptCertificate(question: CertificateQuestion) {
        certificates.value=null
        scope.launch {
            store.edit(question.profileId) { profile -> profile.copy(endpoints=profile.endpoints.map { entry ->
                if(entry.id!=question.endpointId)entry else if(question.audio)entry.copy(audioCertificate=question.fingerprint) else entry.copy(certificate=question.fingerprint)
            }) }
            if(desired==question.profileId || view.value.profileId==question.profileId)connect(question.profileId,question.endpointId)
        }
    }
    fun disconnect(id: String=view.value.profileId) {
        if (id!=view.value.profileId || busy()) { if(busy())message("维护完成后才能断开");return }
        intentEpoch.incrementAndGet();desired="";awaiting=null;switching?.cancel();retry?.cancel()
        setup.stop();setupTimeout?.cancel()
        switching=scope.launch {
            slot.select(null);provision.close();discovery.stop()
            view.value=SessionView();context.stopService(Intent(context,SessionService::class.java))
        }
    }
    fun action(id: String,block: suspend BoardSession.()->Unit) {
        val session=slot.current?.takeIf { it.profile.id==id } ?: return
        if(busy())return
        session.perform(block)
    }
    fun pttDown(id: String) { slot.current?.takeIf{it.profile.id==id && !view.value.maintenance}?.beginPress() }
    fun pttUp(id: String) { slot.current?.takeIf{it.profile.id==id}?.cancelPress() }
    fun toggleListening(id: String)=action(id) { if(view.value.listening)stopListening() else startListening() }
    fun saveDraft(id: String,key: String,value: String) { scope.launch { runCatching { store.draft(id,key,value) }.onFailure { message(it.message ?: "草稿保存失败") } } }
    fun discover(enabled: Boolean) {
        discoveryForList=enabled
        if(enabled)discovery.start() else if(awaiting==null)discovery.stop()
    }
    fun addDiscovered(board: DiscoveredBoard) {
        val known=store.profiles.value.firstOrNull { it.deviceId==board.id }
        val endpoint=AccessPoint(label=board.source,address=board.address,mode=if(board.source.contains("热点"))"HOTSPOT" else "LAN")
        val profile=known?.copy(endpoints=known.endpoints+endpoint,selectedEndpoint=endpoint.id)
            ?: DeviceProfile(name=Brand.discoveryName(board.id),deviceId=board.id,endpoints=listOf(endpoint))
        scope.launch { store.save(profile);connect(profile.id,endpoint.id) }
    }
    private fun onDiscovered(board: DiscoveredBoard) { scope.launch { handleDiscovered(board) } }
    private fun handleDiscovered(board: DiscoveredBoard) {
        discovered.value=(discovered.value.filterNot { it.id==board.id && it.address==board.address }+board).takeLast(100)
        val pending=awaiting ?: return
        if(pending.deviceId!=board.id || desired!=pending.id)return
        awaiting=null
        val endpoint=AccessPoint(label=board.source,address=board.address,mode=if(board.source.contains("热点"))"HOTSPOT" else "LAN")
        val profile=pending.copy(endpoints=pending.endpoints+endpoint,selectedEndpoint=endpoint.id)
        provision.close();connectProfile(profile)
        if(!discoveryForList)discovery.stop()
    }
    fun provisionBoard(existing:DeviceProfile?=null) {
        if(busy())return
        intentEpoch.incrementAndGet();retry?.cancel();switching?.cancel();setupTimeout?.cancel()
        val profile=existing ?: DeviceProfile(name="待配网手台",endpoints=listOf(
            AccessPoint(label="配网热点",address="https://192.168.4.1/",mode="SETUP")))
        setup.start(profile);awaiting=null;desired=profile.id
        val ticket=setup.generation
        switching=scope.launch {
            slot.select(null);view.value=SessionView(phase="请在系统窗口选择手台的配网热点")
            keepService()
            if(Build.VERSION.SDK_INT>=29)provision.request({setupAvailable(it,ticket)},{setupLost(it,ticket)},{setupFailed(it,ticket)})
            else {
                val wifi=provision.availableWifi()
                if(wifi==null)setupFailed("请先在系统 Wi-Fi 设置中连接手台配网热点",ticket)
                else provision.observe(wifi,{setupAvailable(it,ticket)},{setupLost(it,ticket)})
            }
        }
    }
    private fun setupAvailable(network:Network,ticket:Long) { scope.launch {
        if(ticket!=setup.generation)return@launch
        val profile=setup.available(network) ?: return@launch
        setupTimeout?.cancel()
        // Save only once. Subsequent callbacks reuse the canonical identity,
        // certificate and drafts instead of creating a new unverified handset.
        val saved=store.profiles.value.firstOrNull{it.id==profile.id} ?: profile.also{store.save(it)}
        if(ticket!=setup.generation || setup.network!=network)return@launch
        setup.bind(saved)
        connectProfile(saved,network)
    } }
    private fun setupLost(network:Network,ticket:Long) { scope.launch {
        if(ticket!=setup.generation)return@launch
        if(!setup.lost(network,SystemClock.elapsedRealtime()))return@launch
        intentEpoch.incrementAndGet();switching?.cancel();retry?.cancel()
        val profile=setup.profile ?: return@launch
        switching=scope.launch {
            slot.select(null)
            if(ticket==setup.generation && setup.active && setup.network==null) {
                view.value=view.value.copy(profileId=profile.id,deviceId=profile.deviceId,connected=false,
                    phase="配网热点已断开，等待恢复（最多 60 秒）",mine=false,listening=false,ptt=false,pttPending=false,
                    error="请保持靠近手台；系统提示无互联网时可选择保持连接。")
                service?.display()
            }
        }
        setupTimeout?.cancel()
        setupTimeout=scope.launch {
            delay(60_000)
            if(ticket==setup.generation && setup.expired(SystemClock.elapsedRealtime()))setupFailed("配网连接未恢复，请重新点击连接配网热点；已填写内容仍保留",ticket)
        }
    } }
    private fun setupFailed(error:String,ticket:Long) { scope.launch {
        if(ticket!=setup.generation || !setup.active)return@launch
        intentEpoch.incrementAndGet();switching?.cancel();retry?.cancel();setupTimeout?.cancel()
        setup.stop();provision.close();desired=""
        val stopped=setup.generation
        slot.select(null)
        if(stopped!=setup.generation || desired.isNotBlank())return@launch
        view.value=view.value.copy(connected=false,mine=false,listening=false,ptt=false,pttPending=false,
            phase="配网连接已结束",error=error)
        message(error);context.stopService(Intent(context,SessionService::class.java))
    } }
    private fun busy()=view.value.maintenance || slot.current?.view?.value?.maintenance==true
    fun submitNetwork(id: String,ssid: String,password: String,autoJoin: Boolean,priority: Int,onSaved: ()->Unit={}) {
        joinNetwork(id,JSONObject().put("op","save").put("ssid",ssid).put("password",password).put("auto_join",autoJoin).put("priority",priority),null,onSaved)
    }
    fun connectSavedNetwork(id: String,networkId: Long)=joinNetwork(id,null,networkId,{})
    fun submitNetworkProfile(id: String,body: JSONObject,onSaved: ()->Unit)=joinNetwork(id,body,null,onSaved)
    private fun joinNetwork(id: String,save: JSONObject?,networkId: Long?,onSaved: ()->Unit) {
        val current=slot.current?.takeIf{it.profile.id==id} ?: return
        if(busy())return
        val epoch=intentEpoch.incrementAndGet()
        current.perform {
            if(!this.view.value.mine)claim()
            if(epoch!=intentEpoch.get())return@perform
            val savedId=networkId ?: api.json("api/wifi",requireNotNull(save)).getLong("id")
            api.json("api/wifi",JSONObject().put("op","connect").put("id",savedId))
            val pending=profile
            this@DeviceManager.switching=this@DeviceManager.scope.launch {
                if(epoch!=intentEpoch.get())return@launch
                slot.select(null)
                if(epoch!=intentEpoch.get())return@launch
                setup.stop();setupTimeout?.cancel();provision.close();awaiting=pending;desired=pending.id
                onSaved()
                if(pending.endpoint().mode=="PUBLIC") { awaiting=null;delay(2000);connectProfile(pending);return@launch }
                this@DeviceManager.view.value=SessionView(profileId=pending.id,deviceId=pending.deviceId,phase="已保存，等待手台加入目标网络",error="同一手机开热点时，现在开启填写的 2.4 GHz 共享热点。")
                discovery.start();service?.display()
            }
        }
    }
    fun remove(id: String) {
        if(busy() && id==view.value.profileId){message("维护完成后才能移除当前档案");return}
        scope.launch {
            if(id==view.value.profileId){disconnect(id);switching?.join()}
            store.update { all -> all.filterNot { it.id==id } }
        }
    }
    fun serviceStopped() {
        if(slot.current==null && desired.isBlank() && awaiting==null)return
        intentEpoch.incrementAndGet();retry?.cancel();switching?.cancel();desired="";awaiting=null;discovery.stop()
        setup.stop();setupTimeout?.cancel()
        switching=scope.launch {slot.select(null);provision.close();view.value=SessionView(phase="连接服务已停止")}
    }
    fun uploadFile(id: String,uri: Uri,firmware: Boolean)=action(id) {
        val limit=if(firmware)4*1024*1024 else 512*1024
        val bytes=context.contentResolver.openInputStream(uri)?.use { input ->
            val out=java.io.ByteArrayOutputStream();val chunk=ByteArray(8192)
            while(true){
                currentCoroutineContext().ensureActive()
                val n=input.read(chunk);if(n<0)break
                require(out.size()+n<=limit){"文件过大，固件最多 4 MB，尾音文件最多 512 KB"}
                out.write(chunk,0,n)
            };out.toByteArray()
        } ?: throw java.io.IOException("无法读取所选文件")
        require(bytes.isNotEmpty()){ "文件为空" }
        if(firmware)require(bytes.size>=65536 && (bytes[0].toInt() and 255)==0xe9){"请选择 ESP32-S3 应用固件 .bin"}
        upload(bytes,firmware)
    }
    fun exportFirmware(id: String,uri: Uri)=action(id) {
        exportFirmware { bytes ->
            withContext(Dispatchers.IO) { context.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
                ?: throw java.io.IOException("无法写入所选位置") }
        }
    }
    fun diagnosticText(id: String,path: String,result: (String)->Unit)=action(id) {
        val text=api.json(path).toString(2)
        this@DeviceManager.scope.launch { if(slot.current?.profile?.id==id)result(text) }
    }
    fun exportDiagnostics(id: String,uri: Uri)=action(id) {
        refresh(true)
        val snapshot=this.view.value
        val report=JSONObject().put("device_id",api.deviceId).put("endpoint",profile.endpoint().address)
            .put("captured_at_ms",snapshot.updated).put("app_version",BuildConfig.VERSION_NAME)
            .put("status",snapshot.status).put("live",snapshot.live).put("radio",snapshot.radio)
            .put("wifi",snapshot.wifi).put("channels",snapshot.channels).put("control",snapshot.control)
            .put("display_name",profile.displayName).put("custom_name",profile.customName)
            .put("microphone",snapshot.microphoneLevel.let{level->JSONObject()
                .put("input",level.input).put("gain_db",level.gainDb).put("raw_peak",level.rawPeak)
                .put("output_peak",level.outputPeak).put("raw_rms_dbfs",level.rawRmsDb).put("output_rms_dbfs",level.outputRmsDb)
                .put("clipped_samples",level.clipped).put("samples",level.samples)})
        context.contentResolver.openOutputStream(uri)?.use { it.write(report.toString(2).toByteArray(Charsets.UTF_8)) }
            ?: throw java.io.IOException("无法写入诊断文件")
        message("诊断与运行日志已导出")
    }
}
