@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

@Composable internal fun ChannelPage(manager:DeviceManager,p:DeviceProfile,s:SessionView,confirm:(String,()->Unit)->Unit) {
    var query by rememberSaveable(p.id){mutableStateOf("")}
    var favoritesOnly by rememberSaveable(p.id){mutableStateOf(false)}
    var selected by remember(p.id){mutableStateOf<JSONObject?>(null)}
    var naming by remember(p.id){mutableStateOf<JSONObject?>(null)}
    val favoriteKey="ui.channel_favorites"
    val favorites=remember(p.drafts[favoriteKey]){runCatching{val a=JSONArray(p.drafts[favoriteKey] ?: "[]");(0 until a.length()).map{a.getString(it)}.toSet()}.getOrDefault(emptySet())}
    val channels=(0 until s.channels.length()).map{s.channels.getJSONObject(it)}
    fun favorite(c:JSONObject){val key=RadioUiRules.channelKey(c);manager.saveDraft(p.id,favoriteKey,JSONArray((if(key in favorites)favorites-key else favorites+key).toList()).toString())}
    val visible=channels.filter{c->(!favoritesOnly || RadioUiRules.channelKey(c) in favorites) && (query.isBlank() || c.optString("name").contains(query,true) || RadioUiRules.displayFrequency(c.value("rx")).contains(query) || RadioUiRules.displayFrequency(c.value("tx")).contains(query))}
    LazyColumn(Modifier.fillMaxSize(),contentPadding=PaddingValues(20.dp)) {
        item{Row(Modifier.fillMaxWidth().padding(bottom=16.dp),verticalAlignment=Alignment.CenterVertically){Column(Modifier.weight(1f)){Text("信道库",fontSize=29.sp,fontWeight=FontWeight.Medium);Text("${s.channels.length()} / 99 · ${p.displayName}",color=secondary,style=MaterialTheme.typography.bodySmall)}
            IconButton(onClick={naming=JSONObject()},enabled=s.connected&&!s.maintenance&&s.channels.length()<99){Icon(Icons.Default.Add,"保存当前参数为信道")}}}
        item{TextField(query,{query=it},modifier=Modifier.fillMaxWidth(),placeholder={Text("搜索名称或频率")},leadingIcon={Icon(Icons.Default.Search,null,modifier=Modifier.size(19.dp))},singleLine=true,shape=RoundedCornerShape(14.dp),colors=TextFieldDefaults.colors(focusedContainerColor=panel,unfocusedContainerColor=panel,focusedIndicatorColor=androidx.compose.ui.graphics.Color.Transparent,unfocusedIndicatorColor=androidx.compose.ui.graphics.Color.Transparent))}
        item{Row(Modifier.padding(vertical=10.dp),horizontalArrangement=Arrangement.spacedBy(8.dp)){FilterChip(!favoritesOnly,{favoritesOnly=false},label={Text("全部")},shape=RoundedCornerShape(12.dp));FilterChip(favoritesOnly,{favoritesOnly=true},label={Text("收藏")},shape=RoundedCornerShape(12.dp))}}
        if(visible.isEmpty())item{Text(if(channels.isEmpty())"还没有信道。点右上角 +，将手台当前收发参数保存为信道。" else "没有符合条件的信道",color=secondary,modifier=Modifier.padding(vertical=24.dp))}
        items(visible,key={it.optInt("i").toString()+RadioUiRules.channelKey(it)}){c->
            val index=c.optInt("i",channels.indexOf(c));val compatible=c.optBoolean("compatible",true)
            val position=visible.indexOf(c)
            Surface(color=panel,shape=RoundedCornerShape(topStart=if(position==0)20.dp else 0.dp,topEnd=if(position==0)20.dp else 0.dp,bottomStart=if(position==visible.lastIndex)20.dp else 0.dp,bottomEnd=if(position==visible.lastIndex)20.dp else 0.dp)) {
                Column(Modifier.fillMaxWidth().padding(horizontal=14.dp)) {
                    if(position>0)HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
                    Row(Modifier.fillMaxWidth().heightIn(min=84.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                        Text((index+1).toString().padStart(2,'0'),fontSize=13.sp,color=secondary,modifier=Modifier.width(24.dp))
                        Column(Modifier.weight(1f).clickable{selected=c}.padding(vertical=12.dp),verticalArrangement=Arrangement.spacedBy(2.dp)) {
                            Text(c.optString("name"),fontSize=16.sp,lineHeight=23.sp)
                            Text("${RadioUiRules.displayFrequency(c.value("rx"))} · ${c.optString("band")}",fontSize=13.sp,lineHeight=19.sp,color=secondary)
                            if(c.value("rx")!=c.value("tx"))Text("发射 ${RadioUiRules.displayFrequency(c.value("tx"))}",fontSize=12.sp,lineHeight=18.sp,color=secondary)
                            if(!compatible)Text("频段不兼容",color=amber,style=MaterialTheme.typography.bodySmall)
                        }
                        IconButton(onClick={favorite(c)}){Icon(if(RadioUiRules.channelKey(c) in favorites)Icons.Default.Star else Icons.Default.StarBorder,"${if(RadioUiRules.channelKey(c) in favorites)"取消收藏" else "收藏"} ${c.optString("name")}",tint=if(RadioUiRules.channelKey(c) in favorites)blue else secondary,modifier=Modifier.size(18.dp))}
                        FilledTonalButton(onClick={manager.action(p.id){channel("load",index);manager.message("已应用 ${c.optString("name")}")}},enabled=s.connected&&!s.maintenance&&compatible,shape=RoundedCornerShape(15.dp),contentPadding=PaddingValues(horizontal=15.dp,vertical=10.dp),modifier=Modifier.heightIn(min=48.dp)){Text("应用",fontSize=14.sp)}
                    }
                }
            }
        }
    }
    selected?.let{c->ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={selected=null}) {
        Column(Modifier.fillMaxWidth().padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()).verticalScroll(rememberScrollState()).padding(horizontal=24.dp).padding(bottom=32.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
            Text(c.optString("name"),style=MaterialTheme.typography.headlineSmall)
            Text("接收 ${RadioUiRules.displayFrequency(c.value("rx"))} MHz\n发射 ${RadioUiRules.displayFrequency(c.value("tx"))} MHz",fontFamily=FontFamily.Monospace)
            Text("${c.optString("band")} · ${if(c.optInt("bw")==0)"12.5 kHz" else "25 kHz"}\n静噪 ${c.value("sq")} · 音量 ${c.value("vol")} · ${if(c.optInt("pwr")==0)"低功率" else "高功率"}\n发射亚音 ${c.value("txt")} · 接收亚音 ${c.value("rxt")}",color=secondary)
            SettingRow("更改名称",enabled=s.connected&&!s.maintenance){selected=null;naming=c}
            SettingRow("删除信道",subtitle="从手台的信道库移除",enabled=s.connected&&!s.maintenance){selected=null;confirm("删除信道“${c.optString("name")}”？",{manager.action(p.id){channel("del",c.optInt("i"))}})}
        }
    }}
    naming?.let{c->
        val edit=c.has("i")
        var name by remember(c){mutableStateOf(if(edit)c.optString("name") else "")}
        var error by remember(c){mutableStateOf("")};var saving by remember(c){mutableStateOf(false)}
        LaunchedEffect(s.connected,s.maintenance){if(!s.connected||s.maintenance)saving=false}
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={if(!saving)naming=null}) {
            Column(Modifier.fillMaxWidth().padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()).imePadding().verticalScroll(rememberScrollState()).padding(horizontal=24.dp).padding(bottom=32.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
                Text(if(edit)"更改信道名称" else "保存当前参数",style=MaterialTheme.typography.headlineSmall)
                if(!edit)Text("将 ${p.displayName} 的当前频率、亚音、带宽、音量和功率保存到手台。",color=secondary)
                OutlinedTextField(name,{name=it;error=""},label={Text("信道名称")},singleLine=true,modifier=Modifier.fillMaxWidth(),enabled=!saving,isError=error.isNotBlank(),supportingText={Text("${name.toByteArray(Charsets.UTF_8).size} / 23 UTF-8 字节")})
                if(error.isNotBlank())Text(error,color=red)
                Button(onClick={error=RadioUiRules.nameError(name) ?: "";if(error.isBlank()){
                    saving=true;val submitted=name
                    manager.action(p.id){try{
                        channel(if(edit)"edit" else "save",if(edit)c.optInt("i") else null,submitted)
                        if(edit && RadioUiRules.channelKey(c) in favorites){val next=JSONObject(c.toString()).put("name",submitted);manager.store.draft(p.id,favoriteKey,JSONArray((favorites-RadioUiRules.channelKey(c)+RadioUiRules.channelKey(next)).toList()).toString())}
                        withContext(Dispatchers.Main){naming=null;manager.message("信道已保存")}
                    }catch(e:CancellationException){throw e}catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败"};throw e}
                    finally{withContext(Dispatchers.Main){saving=false}}}
                }},enabled=s.connected&&!s.maintenance&&!saving,shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text(if(saving)"正在保存…" else "保存信道")}
            }
        }
    }
}
