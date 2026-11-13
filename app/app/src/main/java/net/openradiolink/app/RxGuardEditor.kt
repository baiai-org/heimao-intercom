@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
package net.openradiolink.app

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable internal fun RxGuardEditor(manager:DeviceManager,p:DeviceProfile,s:SessionView,mode:Int) {
    val config=RxGuardRules.read(s.status,mode)
    var open by remember(p.id,mode){mutableStateOf(false)}
    val ready=s.connected&&!s.maintenance
    Section("发射后接收抑制 · ${if(mode==1)"公网直连" else "网络中继"}") {
        if(config==null) {
            Text("当前固件不支持此功能，请升级配套固件。",color=secondary)
        } else {
            val rg=s.status.optJSONObject("rx_guard")
            val remaining=rg?.optInt("remaining_ms",0) ?: 0
            val blocked=rg?.optInt("blocked",0) ?: 0
            val active=s.status.value("intercom.mode").toIntOrNull()==mode
            val bridgeReady=s.status.value("bridge.active")=="1"
            val status=if(active&&blocked!=0)"接收保护中 · 剩余约 ${RxGuardRules.format(remaining,false)} 秒"
                else if(!config.enabled)"已关闭"
                else if(!active||!bridgeReady)"已启用 · 模拟桥接未就绪"
                else "已启用 · 等待射频发射结束"
            Text(status,color=if(active&&blocked!=0)amber else secondary)
            SettingRow("固定时间保护",if(config.enabled)"${config.milliseconds} ms" else "关闭",enabled=ready){
                manager.pttUp(p.id);open=true
            }
            Text("本机射频发射结束后屏蔽接收，到期恢复；用于避免模拟中继拖尾回传。两种网络模式分别保存。",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
    }
    if(open&&config!=null) {
        var enabled by remember{mutableStateOf(config.enabled)}
        var inMs by remember{mutableStateOf(false)}
        var text by remember{mutableStateOf(RxGuardRules.format(config.milliseconds,false))}
        var sources by remember{mutableIntStateOf(config.sources)}
        var outputs by remember{mutableIntStateOf(config.outputs)}
        var step by remember{mutableIntStateOf(1)}
        var saving by remember{mutableStateOf(false)}
        var error by remember{mutableStateOf("")}
        LaunchedEffect(ready){if(!ready)open=false}
        fun changeTime(ms:Int){text=RxGuardRules.format(ms.coerceIn(1,60000),inMs);error=""}
        fun nudge(direction:Int){runCatching{RxGuardRules.delay(text,inMs)}.onSuccess{changeTime(it+direction*step)}.onFailure{error=it.message ?: "时间无效"}}
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={if(!saving)open=false}) {
            Column(Modifier.fillMaxWidth().fillMaxHeight(.94f).padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
                .imePadding().padding(horizontal=22.dp).padding(bottom=14.dp),verticalArrangement=Arrangement.spacedBy(10.dp)) {
                Text("发射后接收抑制",style=MaterialTheme.typography.headlineSmall)
                Text("${if(mode==1)"公网直连" else "网络中继"} · 固定时间方案",color=secondary)
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState()),verticalArrangement=Arrangement.spacedBy(10.dp)) {
                    Row(verticalAlignment=Alignment.CenterVertically){Switch(enabled,{enabled=it;error=""},enabled=!saving);Text("启用保护",Modifier.padding(start=10.dp))}
                    OutlinedTextField(text,{text=it;error=""},label={Text(if(inMs)"保护时间 · 毫秒" else "保护时间 · 秒")},
                        supportingText={Text(if(inMs)"1–60000，整数毫秒" else "0.001–60.000，最多三位小数")},singleLine=true,
                        keyboardOptions=KeyboardOptions(keyboardType=if(inMs)KeyboardType.Number else KeyboardType.Decimal),
                        enabled=!saving,isError=error.isNotBlank(),modifier=Modifier.fillMaxWidth())
                    Row(verticalAlignment=Alignment.CenterVertically){
                        TextButton(onClick={runCatching{RxGuardRules.delay(text,inMs)}.onSuccess{val next=!inMs;text=RxGuardRules.format(it,next);inMs=next;error=""}.onFailure{error=it.message ?: "时间无效"}},enabled=!saving){Text(if(inMs)"切换为秒" else "切换为毫秒")}
                        TextButton(onClick={nudge(-1)},enabled=!saving){Text("−")}
                        TextButton(onClick={nudge(1)},enabled=!saving){Text("＋")}
                    }
                    Text("微调步进",color=secondary)
                    Row(horizontalArrangement=Arrangement.spacedBy(6.dp)) {
                        listOf(1,10,100,1000).forEach{v->FilterChip(selected=step==v,onClick={step=v},label={Text("$v ms")},enabled=!saving)}
                    }
                    Text("快速填入（仍需保存）",color=secondary)
                    listOf(100,250,500,1000,2000,3000,5000,10000).chunked(4).forEach{row->
                        Row(horizontalArrangement=Arrangement.spacedBy(4.dp)){row.forEach{v->TextButton(onClick={changeTime(v)},enabled=!saving,modifier=Modifier.weight(1f)){Text(if(v<1000)"$v ms" else "${v/1000} 秒")}}}
                    }
                    Text("哪些射频发射结束后触发",style=MaterialTheme.typography.titleMedium)
                    listOf(1 to "网络桥接转发",2 to "APP／网页 PTT",4 to "本机话筒／实体 PTT").forEach{(bit,label)->
                        Row(Modifier.fillMaxWidth().clickable(enabled=!saving){sources=sources xor bit;error=""},verticalAlignment=Alignment.CenterVertically){Checkbox(sources and bit!=0,{sources=sources xor bit;error=""},enabled=!saving);Text(label)}
                    }
                    Text("保护期间屏蔽哪些出口",style=MaterialTheme.typography.titleMedium)
                    listOf(1 to "射频 → 网络转发",2 to "APP／网页收听",4 to "本机喇叭").forEach{(bit,label)->
                        Row(Modifier.fillMaxWidth().clickable(enabled=!saving){outputs=outputs xor bit;error=""},verticalAlignment=Alignment.CenterVertically){Checkbox(outputs and bit!=0,{outputs=outputs xor bit;error=""},enabled=!saving);Text(label)}
                    }
                    if(outputs and 1==0)Text("未屏蔽射频 → 网络转发：仍可能把模拟中继拖尾传回对端。",color=amber)
                    Text("仅模拟桥接启用并就绪时生效。从实际射频 PTT 释放后计时，包含本机尾音发送完毕。保护期间的音频直接丢弃，新讲话也可能被截掉；网络来话与发射继续工作。",color=secondary,style=MaterialTheme.typography.bodySmall)
                    Text("参数支持 1 毫秒步进；实际恢复受音频块和调度影响。到期不等待载波关闭。关闭开关会立即解除保护，其余修改用于下一次发射。",color=secondary,style=MaterialTheme.typography.bodySmall)
                    if(error.isNotBlank())Text(error,color=red)
                }
                Button(onClick={
                    runCatching{RxGuardRules.encode(RxGuardRules.Config(enabled,RxGuardRules.delay(text,inMs),sources,outputs))}
                        .onFailure{error=it.message ?: "设置无效"}.onSuccess{value->
                            saving=true
                            manager.action(p.id){try {
                                set(RxGuardRules.key(mode),value)
                                withContext(Dispatchers.Main){open=false;manager.message("接收抑制设置已保存")}
                            }catch(e:CancellationException){throw e}catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败"};throw e}
                            finally{withContext(Dispatchers.Main){saving=false}}}
                        }
                },enabled=ready&&!saving,modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text(if(saving)"正在保存…" else "保存本模式设置")}
            }
        }
    }
}
