package net.openradiolink.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import org.json.JSONObject

class AppUpdateActivity:ComponentActivity() {
    override fun onCreate(savedInstanceState:Bundle?) {
        super.onCreate(savedInstanceState)
        val app=application as OrlApp
        app.updater.launchCheck()
        setContent {
            val radio by app.manager.view.collectAsStateWithLifecycle()
            val theme=getSharedPreferences("appearance",0).getString("theme","dark") ?: "dark"
            RadioTheme(theme) {
                Surface(Modifier.fillMaxSize()) {
                    Column {
                        TextButton(onClick={finish()}) { Text("返回") }
                        AppUpdatePage(radio)
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun AppUpdatePage(radio:SessionView) {
    val context=LocalContext.current
    val updater=(context.applicationContext as OrlApp).updater
    val state by updater.view.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement=Arrangement.spacedBy(16.dp)) {
        Section("黑猫互联 APP") {
            Text("当前版本 "+BuildConfig.VERSION_NAME)
            state.asset?.let {
                Text("可用版本 "+it.version)
                if(it.notes.isNotBlank())Text(it.notes,color=secondary)
            }
            Text(state.message,color=secondary)
            if(state.ready && (radio.ptt || radio.pttPending || radio.listening || radio.maintenance)) {
                Text("通联或设备维护结束后可安装",color=secondary)
            }
            FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick={updater.checkNow()},enabled=!state.busy){Text("检查 APP 更新")}
                val asset=state.asset
                if(state.ready && asset!=null && (!asset.force || state.systemConfirmation)) {
                    Button(onClick={updater.install(context)},enabled=!radio.ptt && !radio.pttPending &&
                        !radio.listening && !radio.maintenance) {
                        Text(if(state.systemConfirmation)"继续安装" else "立即安装")
                    }
                    if(!asset.force && !state.systemConfirmation)TextButton(onClick={updater.later()}){Text("稍后")}
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun BoardReleaseUpdateSection(manager:DeviceManager,p:DeviceProfile,s:SessionView,
    confirm:(String,()->Unit)->Unit) {
    var status by remember(p.id,s.connected){mutableStateOf<JSONObject?>(null)}
    var message by remember(p.id){mutableStateOf("")}
    var unavailable by remember(p.id,s.connected){mutableStateOf(false)}
    var busy by remember(p.id){mutableStateOf(false)}
    LaunchedEffect(p.id,s.connected) {
        if(!s.connected)return@LaunchedEffect
        while(true) {
            if(!busy)manager.action(p.id) {
                try {
                    status=api.json("api/online_update")
                    unavailable=false
                } catch(e:CancellationException){throw e}
                catch(e:ApiFailure){unavailable=e.status==404}
                catch(_:Exception){ }
            }?.join()
            delay(5000)
        }
    }
    fun request(op:String,snapshot:JSONObject) {
        busy=true
        val job=manager.action(p.id) {
            try {
                status=api.json("api/online_update",JSONObject().put("op",op)
                    .put("boot_id",snapshot.getString("boot_id")).put("sha256",snapshot.optString("sha256")))
                message=if(op=="install")"安装请求已接收" else "已请求检查"
            } catch(e:CancellationException){throw e}
            catch(e:Exception){message=e.message ?: "操作未完成"}
            finally {busy=false}
        }
        if(job==null)busy=false
    }
    Section("手台在线更新") {
        val snapshot=status
        val code=snapshot?.optString("state")
        val hint=when(code) {
            "no_manifest"->"当前正式发布未提供更新清单"
            "current"->"手台已是最新可用版本"
            "checking"->"正在检查 GitHub"
            "downloading"->"正在下载新版"
            "waiting_idle"->"等待通联结束后写入备用槽"
            "staging"->"正在写入备用槽"
            "ready"->"新版已下载，可立即安装或稍后处理"
            "force_wait"->"新版已下载，手台空闲后自动安装"
            "rollback_required"->"新版已下载，当前引导程序需要手动安装"
            "install_wait"->"已安排安装，等待手台空闲"
            "rebooting"->"正在重启并安装"
            "rolled_back"->"新版启动失败，手台已回退"
            "error"->"检查或下载未完成，可重试"
            else->if(unavailable)"当前固件未提供在线更新" else "等待手台联网与校时"
        }
        Text(hint,color=secondary)
        snapshot?.optString("version")?.takeIf{it.isNotBlank()}?.let{Text("可用版本 "+it)}
        snapshot?.optString("notes")?.takeIf{it.isNotBlank()}?.let{Text(it,color=secondary)}
        if(message.isNotBlank())Text(message,color=secondary)
        if(snapshot!=null) FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            val available=s.connected&&!busy&&!s.maintenance
            OutlinedButton(onClick={request("check",snapshot)},enabled=available){Text("检查手台更新")}
            if(snapshot.optBoolean("install")) {
                Button(onClick={confirm("安装手台新版并重启？手台将保留原有设置并等待通联结束。",{request("install",snapshot)})},
                    enabled=available&&!s.ptt&&!s.pttPending&&!s.listening){Text("立即安装")}
                TextButton(onClick={message="已保留下载，可稍后安装"}){Text("稍后")}
            }
        }
    }
}
