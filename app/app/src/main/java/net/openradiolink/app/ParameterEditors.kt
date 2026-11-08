@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
package net.openradiolink.app

import androidx.compose.foundation.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.*
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable internal fun Parameter(manager:DeviceManager,profile:DeviceProfile,state:SessionView,key:String,label:String,current:String,
    options:List<Pair<String,String>> = emptyList(),hint:String="",secret:Boolean=false) {
    var open by remember(profile.id,key){mutableStateOf(false)}
    val value=if(secret){if(current.isBlank())"未设置" else "已设置"} else options.firstOrNull{it.first==current}?.second ?: current.ifBlank{"等待更新"}
    SettingRow(label,value,enabled=state.connected&&!state.maintenance){manager.pttUp(profile.id);open=true}
    if(open) {
        var text by remember{mutableStateOf(if(secret)"" else profile.drafts[key] ?: current)}
        var error by remember{mutableStateOf("")};var saving by remember{mutableStateOf(false)}
        LaunchedEffect(state.connected,state.maintenance){if(!state.connected || state.maintenance)saving=false}
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={if(!saving)open=false}) {
            Column(Modifier.fillMaxWidth().fillMaxHeight(.92f)
                .padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
                .imePadding().padding(horizontal=22.dp).padding(bottom=14.dp),
                verticalArrangement=Arrangement.spacedBy(12.dp)) {
                Text(label,style=MaterialTheme.typography.headlineSmall)
                Text("当前：$value",color=secondary)
                /* The list scrolls; the save button below stays put, however many
                 * tones (or channels) the option list holds. */
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState()),verticalArrangement=Arrangement.spacedBy(10.dp)) {
                    if(key=="tail_modes") {
                        val mask=text.toIntOrNull() ?: 0
                        listOf(1 to "本地电台",2 to "公网直连",4 to "网络中继").forEach{(bit,title)->
                            Row(Modifier.fillMaxWidth().clickable(enabled=!saving){text=(mask xor bit).toString();error=""},verticalAlignment=Alignment.CenterVertically){Checkbox(mask and bit != 0,{text=(mask xor bit).toString();error=""},enabled=!saving);Text(title)}
                        }
                    } else if(options.isNotEmpty()) {
                        options.forEach{(id,title)->Row(Modifier.fillMaxWidth().heightIn(min=52.dp).clickable(enabled=!saving){text=id;error=""},verticalAlignment=Alignment.CenterVertically){RadioButton(selected=text==id,onClick={text=id;error=""},enabled=!saving);Text(title,modifier=Modifier.padding(start=6.dp))}}
                    } else OutlinedTextField(text,{text=it;error="";if(!secret)manager.saveDraft(profile.id,key,it)},label={Text(label)},modifier=Modifier.fillMaxWidth(),singleLine=key!="tail_custom_s",
                        isError=error.isNotBlank(),visualTransformation=if(secret)PasswordVisualTransformation() else VisualTransformation.None,
                        keyboardOptions=KeyboardOptions(keyboardType=if(secret)KeyboardType.Password else if(key in listOf("sq","vol","gain","txgain","micgain","adcvol","ix_room"))KeyboardType.Number else KeyboardType.Text))
                    if(hint.isNotBlank())Text(hint,color=secondary,style=MaterialTheme.typography.bodyMedium)
                    if(secret)Text("留空保持原密码；清除密码需使用页面上的独立操作。",color=secondary,style=MaterialTheme.typography.bodySmall)
                    if(error.isNotBlank())Text(error,color=red)
                }
                Button(onClick={
                    error=RadioUiRules.parameterError(key,text) ?: if(key=="tail_modes" && text.toIntOrNull() !in 1..7)"请至少选择一种模式" else ""
                    if(error.isBlank()){
                        saving=true;val submitted=text
                        manager.action(profile.id){try{
                            set(key,submitted);manager.store.clearDraft(profile.id,key)
                            withContext(Dispatchers.Main){open=false;manager.message("$label 已保存")}
                        }catch(e:CancellationException){throw e}catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败"};throw e}
                        finally{withContext(Dispatchers.Main){saving=false}}}
                    }
                },enabled=state.connected&&!state.maintenance&&!saving&&(!secret||text.isNotBlank()),shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){
                    Text(if(saving)"正在保存…" else "保存")
                }
            }
        }
    }
}

@Composable internal fun FrequencyEditor(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var open by remember(p.id){mutableStateOf(false)}
    SettingRow("收发频率","RX ${RadioUiRules.displayFrequency(s.status.value("freq.rx"))} / TX ${RadioUiRules.displayFrequency(s.status.value("freq.tx"))} MHz",enabled=s.connected&&!s.maintenance){manager.pttUp(p.id);open=true}
    if(open){
        var tx by remember{mutableStateOf(p.drafts["frequency.tx"] ?: s.status.value("freq.tx"))}
        var rx by remember{mutableStateOf(p.drafts["frequency.rx"] ?: s.status.value("freq.rx"))}
        var same by remember{mutableStateOf(false)};var error by remember{mutableStateOf("")};var saving by remember{mutableStateOf(false)}
        LaunchedEffect(s.connected,s.maintenance){if(!s.connected || s.maintenance)saving=false}
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={if(!saving)open=false}) {
            Column(Modifier.fillMaxWidth().padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()).imePadding().verticalScroll(rememberScrollState()).padding(horizontal=24.dp).padding(bottom=32.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
                Text("收发频率",style=MaterialTheme.typography.headlineSmall)
                Text("${s.radio.value("band")} · ${s.radio.value("min")}–${s.radio.value("max")} MHz\n12.5 kHz 步进，保存后以手台端读回为准。",color=secondary)
                OutlinedTextField(rx,{rx=it;manager.saveDraft(p.id,"frequency.rx",it);if(same){tx=it;manager.saveDraft(p.id,"frequency.tx",it)};error=""},label={Text("接收 RX · MHz")},singleLine=true,enabled=!saving,modifier=Modifier.fillMaxWidth(),keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Decimal))
                OutlinedTextField(tx,{tx=it;manager.saveDraft(p.id,"frequency.tx",it);error=""},label={Text("发射 TX · MHz")},singleLine=true,enabled=!saving&&!same,modifier=Modifier.fillMaxWidth(),keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Decimal))
                Row(verticalAlignment=Alignment.CenterVertically){Switch(same,{same=it;if(it){tx=rx;manager.saveDraft(p.id,"frequency.tx",rx)}},enabled=!saving);Text("收发同频",modifier=Modifier.padding(start=12.dp))}
                if(error.isNotBlank())Text(error,color=red)
                Button(onClick={
                    runCatching{RadioUiRules.frequency(tx,s.radio.value("min"),s.radio.value("max")) to RadioUiRules.frequency(rx,s.radio.value("min"),s.radio.value("max"))}
                        .onFailure{error=it.message ?: "频率无效"}.onSuccess{(nextTx,nextRx)->
                            saving=true;manager.action(p.id){try{
                                set("freq_pair","$nextTx,$nextRx");manager.store.clearDraft(p.id,"frequency.tx","frequency.rx")
                                withContext(Dispatchers.Main){open=false;manager.message("收发频率已保存")}
                            }catch(e:CancellationException){throw e}catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败"};throw e}
                            finally{withContext(Dispatchers.Main){saving=false}}}
                        }
                },enabled=s.connected&&!s.maintenance&&!saving,shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text(if(saving)"正在保存…" else "保存这组频率")}
            }
        }
    }
}


