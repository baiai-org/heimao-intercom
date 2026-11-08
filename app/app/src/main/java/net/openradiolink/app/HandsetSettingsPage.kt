package net.openradiolink.app

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlin.math.roundToInt

@Composable internal fun HandsetNameFields(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var input by remember(p.id){mutableStateOf(p.customName)}
    var expected by remember(p.id){mutableLongStateOf(p.nameRevision)}
    var dirty by remember(p.id){mutableStateOf(false)}
    var saving by remember(p.id){mutableStateOf(false)}
    var result by remember(p.id){mutableStateOf("")}
    var reviewed by remember(p.id){mutableLongStateOf(p.nameRevision)}
    LaunchedEffect(p.nameRevision,p.customName) {
        if(!dirty){input=p.customName;expected=p.nameRevision}
        else if(p.nameRevision!=reviewed){result="名称已更新，请核对后点击保存";expected=p.nameRevision}
        reviewed=p.nameRevision
    }
    Section("手台名称") {
        Text(p.displayName,style=MaterialTheme.typography.titleMedium)
        Text("设备尾号 ${p.deviceId.takeLast(6).uppercase()} · ${if(s.connected)"已连接" else "离线缓存"}",color=secondary)
        if(p.nameRevision<0)Text("连接新版手台固件后，可设置网页与 APP 共享的名称。",color=secondary)
        else {
            OutlinedTextField(input,{input=it;dirty=true;result=""},label={Text("共享名称 · 最多 24 字")},
                enabled=!saving,modifier=Modifier.fillMaxWidth(),singleLine=true)
            Text("名称保存在手台，所有客户端共用；清空恢复默认。",color=secondary)
            Text("当前手台名称：${p.customName.ifBlank{"未设置"}}",color=secondary)
            Button(onClick={
                val value=runCatching{HandsetName.normalized(input)}.getOrElse{result=it.message.orEmpty();return@Button}
                saving=true
                manager.action(p.id) {
                    try { renameHandset(value,expected);dirty=false;result="已保存到手台";manager.message(result) }
                    finally { saving=false }
                }
            },enabled=s.connected&&!s.maintenance&&!saving){Text(if(saving)"保存中…" else "保存手台名称")}
            TextButton(onClick={input=p.name;dirty=true;result=""},enabled=!saving){Text("使用本机备注名")}
        }
        Text("本机备注名：${p.name}（仅此手机）",color=secondary)
        if(result.isNotBlank())Text(result,color=secondary)
    }
}

@Composable internal fun MicrophonePage(manager:DeviceManager,s:SessionView) {
    val context=LocalContext.current
    val scope=rememberCoroutineScope()
    val settings=remember{MicrophoneSettings(context)}
    val test=remember{LocalMicrophoneTest(context,scope)}
    val state by test.view.collectAsStateWithLifecycle()
    val recent=remember{settings.lastInput()}
    var key by remember{mutableStateOf(recent.first)}
    var label by remember{mutableStateOf(recent.second)}
    var gain by remember{mutableIntStateOf(if(key.isBlank())0 else settings.gain(key))}
    var message by remember{mutableStateOf("")}
    val lifecycle=LocalLifecycleOwner.current
    DisposableEffect(lifecycle) {
        val observer=LifecycleEventObserver{_,event->if(event==Lifecycle.Event.ON_PAUSE)test.stop()}
        lifecycle.lifecycle.addObserver(observer)
        onDispose{lifecycle.lifecycle.removeObserver(observer);test.stop()}
    }
    LaunchedEffect(state.key){if(state.key.isNotBlank()){key=state.key;label=state.input}}
    LaunchedEffect(s.listening){if(s.listening)test.stop()}
    val permission=rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()){granted->
        if(granted)message="麦克风已允许，请点击开始测试" else message="麦克风测试需要录音权限"
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),verticalArrangement=Arrangement.spacedBy(18.dp)) {
        Section("APP 发话音量") {
            Text("+$gain dB",style=MaterialTheme.typography.headlineSmall)
            Text("$label\n仅此手机与该录音输入设备 · 下次发话生效",color=secondary)
            Slider(value=gain.toFloat(),onValueChange={gain=((it/3).roundToInt()*3).coerceIn(0,12);test.invalidate()},
                valueRange=0f..12f,steps=3,enabled=!state.running&&!state.playing)
            Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                listOf(0 to "原始",6 to "标准",12 to "增强").forEach{(v,title)->
                    FilterChip(selected=gain==v,onClick={gain=v;test.invalidate()},label={Text(title)},enabled=!state.running&&!state.playing)
                }
            }
            Text("+6 dB 约两倍幅度。未校准输入保持 0 dB；先试听，再在对端确认音量与失真。",color=secondary)
            Button(onClick={runCatching{settings.save(key,gain)}.onSuccess{message="已保存，下次发话生效"}.onFailure{message=it.message.orEmpty()}},
                enabled=key.isNotBlank()&&!state.running&&!state.playing){Text("保存发话音量")}
        }
        Section("本机麦克风测试") {
            Text("录制 5 秒，仅在手机内存试听；退出或离开前台即清除。",color=secondary)
            if(s.listening)Text("请先停止当前手台的收听，再测试麦克风。",color=secondary)
            Button(onClick={
                if(ContextCompat.checkSelfPermission(context,Manifest.permission.RECORD_AUDIO)==PackageManager.PERMISSION_GRANTED)test.record(gain)
                else permission.launch(Manifest.permission.RECORD_AUDIO)
            },enabled=!s.listening&&!state.running&&!state.playing){Text(if(state.running)"正在录音…" else "开始 5 秒测试")}
            if(state.running || state.complete)MicrophoneMeter(state.level)
            Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick={test.play()},enabled=state.complete&&!state.playing&&!state.running&&!s.listening){Text("试听")}
                TextButton(onClick={test.stop()}){Text("停止并清除")}
            }
            Text("本地试听用于检查输入，最终以接收端声音为准。",color=secondary)
            if(state.error.isNotBlank())Text(state.error,color=red)
        }
        if(message.isNotBlank())Text(message,color=secondary)
    }
}

@Composable internal fun MicrophoneMeter(level:MicrophoneLevel,boardClipped:Boolean=false) {
    Column(verticalArrangement=Arrangement.spacedBy(6.dp)) {
        Text(if(boardClipped)"手台输出过载，请调低发话音量" else "麦克风 · ${level.label}",color=if(boardClipped || level.clipped>0)red else secondary)
        LinearProgressIndicator(progress={level.fraction},modifier=Modifier.fillMaxWidth())
        Text("${level.input} · +${level.gainDb} dB",style=MaterialTheme.typography.bodySmall,color=secondary)
    }
}
