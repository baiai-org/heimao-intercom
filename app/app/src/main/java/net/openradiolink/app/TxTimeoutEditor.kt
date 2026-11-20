package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun TxTimeoutEditor(
    manager: DeviceManager,
    profile: DeviceProfile,
    state: SessionView,
    drafts: MutableMap<String, String>,
    reveal: Boolean,
    onRevealed: () -> Unit,
) {
    val device = state.deviceId
    val draft = drafts[device]
    val current = RadioUiRules.txTimeoutSeconds(state.radio)
        ?.takeIf { state.connected && !state.txTimeoutReadFailed && state.txTimeoutSupported == true }
    val text = draft ?: current?.toString().orEmpty()
    val forwarding = state.status.optBoolean("tx_timeout_busy") ||
        state.status.optJSONObject("bridge")?.value("rx_open") in listOf("1", "true")
    val transmitting = state.pttPending ||
        RadioStateRules.activity(state.connected,state.ptt,state.status,state.activity.sample) == RadioActivity.TRANSMIT
    val maintenance = state.maintenance || state.status.optBoolean("tx_timeout_maintenance") || state.deviceMaintenance.optBoolean("reboot_pending") ||
        state.deviceMaintenance.optString("busy") == "writing"
    var working by remember(device) { mutableStateOf(false) }
    var localError by remember(device) { mutableStateOf("") }
    var focused by remember(device) { mutableStateOf(false) }
    val saving = working || state.txTimeoutSaving
    val editable = current != null && device.isNotBlank() && !saving
    val focusRequester = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current
    val inView = remember { BringIntoViewRequester() }
    val keyboardHeight = WindowInsets.ime.getBottom(LocalDensity.current)

    LaunchedEffect(reveal) {
        if (reveal) {
            inView.bringIntoView()
            if (editable) focusRequester.requestFocus()
            onRevealed()
        }
    }
    LaunchedEffect(focused, keyboardHeight) { if (focused) inView.bringIntoView() }

    fun save() {
        if (!editable || draft == null || transmitting || forwarding || maintenance || working) return
        localError = RadioUiRules.parameterError("tx_timeout_s", draft).orEmpty()
        if (localError.isNotEmpty()) return
        val seconds = draft.toInt()
        focusManager.clearFocus()
        working = true
        val job = manager.action(profile.id) {
            if (api.deviceId == device && saveTxTimeout(seconds)) {
                withContext(Dispatchers.Main) { if (drafts[device] == draft) drafts.remove(device) }
            }
        }
        if (job == null) {
            working = false
            localError = "当前手台不可写，请稍后重试"
        } else job.invokeOnCompletion { manager.scope.launch { working = false } }
    }

    val feedback = when {
        saving -> "正在保存…"
        state.txTimeoutSupported == false -> "当前手台固件不支持修改"
        !state.connected -> "正在连接手台…"
        state.txTimeoutReadFailed -> "发射超时读取失败"
        current == null -> "正在读取发射超时设置…"
        maintenance -> "请结束维护后保存"
        transmitting || forwarding -> "请结束发射或转发后保存"
        localError.isNotEmpty() -> localError
        draft != null && state.txTimeoutResult.startsWith("已保存") -> ""
        else -> state.txTimeoutResult
    }
    val status = listOfNotNull(feedback.takeIf { it.isNotEmpty() },
        "未保存".takeIf { draft != null }, current?.let { "当前生效 $it 秒" }).joinToString(" · ")

    Surface(color=panel, shape=RoundedCornerShape(16.dp), modifier=Modifier.fillMaxWidth().bringIntoViewRequester(inView)) {
        Column(Modifier.padding(horizontal=12.dp, vertical=8.dp), verticalArrangement=Arrangement.spacedBy(5.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment=Alignment.CenterVertically,
                horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(text, { drafts[device]=it; localError="" }, enabled=editable,
                    label={ Text("发射超时") }, suffix={ Text("秒") }, singleLine=true, isError=localError.isNotEmpty(),
                    modifier=Modifier.weight(1f).focusRequester(focusRequester).onFocusChanged { focused=it.isFocused },
                    keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number, imeAction=ImeAction.Done),
                    keyboardActions=KeyboardActions(onDone={ focusManager.clearFocus() }))
                Button(onClick=::save, enabled=editable && draft != null && !transmitting && !forwarding && !maintenance,
                    modifier=Modifier.heightIn(min=48.dp)) { Text("保存") }
            }
            Text(status, color=if(localError.isBlank())secondary else red, style=MaterialTheme.typography.bodySmall,
                modifier=Modifier.semantics { liveRegion=LiveRegionMode.Polite })
            if (state.connected && state.txTimeoutReadFailed) {
                TextButton(onClick={ manager.action(profile.id) { if (api.deviceId == device) readTxTimeout() } }) {
                    Text("重试读取")
                }
            } else Text("可输入 5–600 的整数，默认 180 秒", color=secondary, style=MaterialTheme.typography.bodySmall)
        }
    }
}
