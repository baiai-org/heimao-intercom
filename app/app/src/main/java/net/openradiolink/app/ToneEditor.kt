@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
package net.openradiolink.app

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.nio.charset.Charset

/** 0000/空 = 关闭；三码+N/I = 数字；其余按模拟亚音码表解释。 */
internal fun toneKind(code:String):String {
    val v=code.trim().uppercase()
    return when {
        v.isEmpty() || v=="0000" -> "off"
        Regex("^[0-7]{3}[NI]").matches(v) && v.length == 4 -> "dcs"
        else -> "ctcss"
    }
}

private fun toneLabel(code:String,tones:List<Pair<String,String>>):String = when(toneKind(code)) {
    "off" -> "关闭亚音"
    "dcs" -> "DCS "+code.dropLast(1)+" "+(if(code.endsWith("I",true)) "反相" else "正相")
    else -> tones.firstOrNull{it.first==code.uppercase()}?.second ?: code.ifBlank{"未设置"}
}

@Composable private fun ToneRow(title:String,selected:Boolean,click:()->Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min=50.dp).clickable(onClick=click),verticalAlignment=Alignment.CenterVertically) {
        RadioButton(selected=selected,onClick=click)
        Text(title,modifier=Modifier.padding(start=6.dp),style=MaterialTheme.typography.bodyLarge)
    }
}

/** 发射/接收亚音：类型三选一，参数按类型出现；保存按钮固定在弹层底部。 */
@Composable internal fun ToneEditor(manager:DeviceManager,p:DeviceProfile,s:SessionView,label:String,direction:String) {
    val current=s.status.value("freq.ctcss_${direction}")
    var tones by remember(p.id){mutableStateOf(listOf<Pair<String,String>>())}
    var codes by remember(p.id){mutableStateOf(listOf<String>())}
    LaunchedEffect(p.id,s.connected){ if(s.connected) manager.action(p.id){
        val t=JSONArray(String(api.bytes("api/ctcss",32768),Charsets.UTF_8))
        val d=JSONArray(String(api.bytes("api/dcs",32768),Charsets.UTF_8))
        val tl=mutableListOf<Pair<String,String>>();repeat(t.length()){val o=t.getJSONObject(it);tl+=o.getString("code") to o.optString("label",o.getString("code"))}
        val dl=mutableListOf<String>();repeat(d.length()){dl+=d.getJSONObject(it).getString("code")}
        manager.scope.launch { tones=tl;codes=dl }
    }}
    var open by remember(p.id){mutableStateOf(false)}
    SettingRow(label,toneLabel(current,tones),enabled=s.connected&&!s.maintenance){manager.pttUp(p.id);open=true}
    if(open) {
        var kind by remember{mutableStateOf(toneKind(current))}
        var tone by remember{mutableStateOf(if(toneKind(current)=="ctcss") current.uppercase() else "")}
        var code by remember{mutableStateOf(if(toneKind(current)=="dcs") current.dropLast(1) else "")}
        var inverted by remember{mutableStateOf(current.endsWith("I",true))}
        var saving by remember{mutableStateOf(false)};var error by remember{mutableStateOf("")}
        LaunchedEffect(s.connected,s.maintenance){if(!s.connected || s.maintenance)saving=false}
        fun compose():String = when(kind) {
            "off" -> "0000"
            "ctcss" -> tone.ifBlank{tones.firstOrNull()?.first ?: "0001"}
            else -> code.ifBlank{codes.firstOrNull() ?: "023"}+if(inverted)"I" else "N"
        }
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={if(!saving)open=false}) {
            Column(Modifier.fillMaxWidth().fillMaxHeight(.92f)
                .padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
                .imePadding().padding(horizontal=22.dp).padding(bottom=14.dp),
                verticalArrangement=Arrangement.spacedBy(12.dp)) {
                Text(label,style=MaterialTheme.typography.headlineSmall)
                Text("当前：${toneLabel(current,tones)}",color=secondary)
                Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    listOf("off" to "关闭","ctcss" to "模拟 CTCSS","dcs" to "数字 DCS").forEach{(k,t)->
                        FilledTonalButton(onClick={
                            kind=k;error=""
                            if(k=="ctcss"&&tone.isBlank())tone=tones.firstOrNull()?.first ?: "0001"
                            if(k=="dcs"&&code.isBlank())code=codes.firstOrNull() ?: "023"
                        },enabled=!saving,shape=RoundedCornerShape(14.dp),modifier=Modifier.weight(1f),
                            colors=ButtonDefaults.filledTonalButtonColors(containerColor=if(kind==k)MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant),
                            contentPadding=PaddingValues(horizontal=6.dp,vertical=10.dp)){Text(t,style=MaterialTheme.typography.labelMedium,maxLines=1)}
                    }
                }
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                    when(kind) {
                        "ctcss" -> tones.forEach{(id,title)->ToneRow(title,selected=tone.ifBlank{compose()}==id){tone=id}}
                        "dcs" -> codes.forEach{c->ToneRow("DCS "+c,selected=code.ifBlank{compose().dropLast(1)}==c){code=c}}
                        else -> Text("关闭后不发送、也不按亚音接收。",color=secondary)
                    }
                }
                if(kind=="dcs") Row(verticalAlignment=Alignment.CenterVertically) {
                    Switch(inverted,{inverted=it},enabled=!saving)
                    Text(if(inverted)"反相 I" else "正相 N",modifier=Modifier.padding(start=8.dp))
                }
                if(error.isNotBlank())Text(error,color=red)
                Button(onClick={
                    val v=compose();saving=true
                    manager.action(p.id){try{
                        set("ctcss_${direction}",v)
                        withContext(Dispatchers.Main){open=false;manager.message("${label} 已保存")}
                    }catch(e:CancellationException){throw e}catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败"};throw e}
                    finally{withContext(Dispatchers.Main){saving=false}}}
                },enabled=s.connected&&!s.maintenance&&!saving,shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text(if(saving)"正在保存…" else "保存")}
            }
        }
    }
}
