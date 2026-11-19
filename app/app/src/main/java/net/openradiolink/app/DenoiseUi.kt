@file:OptIn(
    androidx.compose.material3.ExperimentalMaterial3Api::class,
    androidx.compose.foundation.layout.ExperimentalLayoutApi::class,
)

package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun DenoiseEditor(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var open by remember(p.id){mutableStateOf(false)}
    val connected=s.profileId==p.id && s.connected && !s.maintenance
    SettingRow("语音降噪",value="分线路档位",enabled=connected) { open=true }
    if(open) {
        ModalBottomSheet(
            sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),
            onDismissRequest={open=false},
        ) {
            DenoiseSheet(manager,p.id,s)
        }
    }
}

@Composable
private fun DenoiseSheet(manager:DeviceManager,profileId:String,s:SessionView) {
    val scope=rememberCoroutineScope()
    var saved by remember{mutableStateOf<DenoiseSettings?>(null)}
    val drafts=remember{mutableStateMapOf<DenoisePath,DenoiseDraft>()}
    var busy by remember{mutableStateOf(false)}
    var message by remember{mutableStateOf("")}
    var error by remember{mutableStateOf(false)}
    var automaticReadKey by remember{mutableStateOf<Pair<String,Int?>?>(null)}
    val currentMode=RadioStateRules.mode(s.activity.live(s.live),s.status)
    val connected=s.profileId==profileId && s.connected && !s.maintenance
    val current=saved
    val paths=current?.paths.orEmpty()
    val editable=connected && !busy && current!=null && (currentMode==null || currentMode==current.mode)
    val changes=paths.mapNotNull { path->drafts[path]?.let{path to it} }.toMap()

    fun run(operation:suspend BoardSession.()->Unit) {
        if(busy)return
        busy=true
        message=""
        error=false
        val job=manager.action(profileId) {
            try {
                operation()
            } catch(e:CancellationException) {
                throw e
            } catch(e:Exception) {
                withContext(Dispatchers.Main) {
                    if(e is DenoiseUnsupported) saved=null
                    error=true
                    message=e.message ?: "降噪设置未确认，请重试"
                }
                if(e is CertificateFailure)throw e
            }
        }
        if(job==null) {
            busy=false
            message="手台当前不可操作，请重新读取降噪设置"
            error=true
        } else {
            job.invokeOnCompletion { scope.launch { busy=false } }
        }
    }

    fun read()=run {
        val result=api.readDenoise()
        withContext(Dispatchers.Main){saved=result}
    }

    LaunchedEffect(connected,s.deviceId,currentMode,busy) {
        if(!connected) {
            automaticReadKey=null
        } else if(!busy) {
            val key=s.deviceId to currentMode
            if(automaticReadKey!=key) {
                automaticReadKey=key
                read()
            }
        }
    }

    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
            .padding(horizontal=20.dp).padding(bottom=24.dp),
        verticalArrangement=Arrangement.spacedBy(10.dp),
    ) {
        Text("语音降噪",style=MaterialTheme.typography.headlineSmall)
        if(current!=null) {
            Text(RadioStateRules.modeName(current.mode),color=secondary)
            for(path in paths) {
                val stored=current.level(path)
                val draft=drafts[path]
                val value=draft?.requested ?: stored
                DenoiseLevelRow(path,value,stored,editable) { level->
                    if(level==stored) drafts.remove(path)
                    else drafts[path]=DenoiseDraft(draft?.expected ?: stored,level)
                    message=""
                    error=false
                }
            }
            Text("保存后从下一话段生效。",color=secondary,style=MaterialTheme.typography.bodySmall)
            if(paths.any{(drafts[it]?.requested ?: current.level(it))==DenoiseLevel.EXTREME}) {
                Text("极强档可能削弱弱话音和尾音。",color=amber,style=MaterialTheme.typography.bodySmall)
            }
        }

        if(busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if(message.isNotBlank()) Text(message,color=if(error)red else secondary)
        if(changes.isNotEmpty()) Text("有未保存修改",color=amber,style=MaterialTheme.typography.bodySmall)
        FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick={read()},enabled=connected&&!busy){Text("重新读取")}
            if(current!=null) {
                TextButton(
                    onClick={paths.forEach{drafts.remove(it)};message="";error=false},
                    enabled=editable&&changes.isNotEmpty(),
                ){Text("撤销修改")}
                Button(onClick={
                    val write=DenoiseWrite(current.mode,changes)
                    run {
                        val result=api.writeDenoise(write)
                        withContext(Dispatchers.Main) {
                            saved=result
                            if(write.confirmed(result)) {
                                write.changes.keys.forEach{drafts.remove(it)}
                                message="已保存，下一话段生效"
                            } else {
                                val conflicts=mutableListOf<String>()
                                write.changes.forEach { (path,draft) ->
                                    val latest=result.level(path)
                                    if(latest==draft.requested) {
                                        drafts.remove(path)
                                    } else {
                                        drafts[path]=draft.copy(expected=latest)
                                        if(latest!=draft.expected) conflicts.add(path.label+"为"+latest.label)
                                    }
                                }
                                error=true
                                message=if(conflicts.isEmpty()) "保存未确认，草稿已保留"
                                else "手台设置有变化（"+conflicts.joinToString("、")+"），请核对后再保存"
                            }
                        }
                    }
                },enabled=editable&&changes.isNotEmpty()){Text("保存")}
            }
        }
    }
}

@Composable
private fun DenoiseLevelRow(
    path:DenoisePath,
    level:DenoiseLevel,
    stored:DenoiseLevel,
    enabled:Boolean,
    change:(DenoiseLevel)->Unit,
) {
    var expanded by remember(path){mutableStateOf(false)}
    Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,
        horizontalArrangement=Arrangement.spacedBy(10.dp)) {
        Column(Modifier.weight(1f)) {
            Text(path.label)
            if(level!=stored) Text("已保存 "+stored.label,color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        Box {
            OutlinedButton(onClick={expanded=true},enabled=enabled){Text(level.label)}
            DropdownMenu(expanded=expanded&&enabled,onDismissRequest={expanded=false}) {
                DenoiseLevel.entries.forEach { choice ->
                    DropdownMenuItem(text={Text(choice.label)},onClick={expanded=false;change(choice)})
                }
            }
        }
    }
}
