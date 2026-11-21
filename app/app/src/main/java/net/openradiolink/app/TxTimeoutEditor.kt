package net.openradiolink.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TxTimeoutSetting(
    manager: DeviceManager,
    profile: DeviceProfile,
    state: SessionView,
    drafts: MutableMap<String, String>,
) {
    val device = state.deviceId
    val retryable = state.connected && state.txTimeoutSupported == true && state.txTimeoutReadFailed
    val current = RadioUiRules.txTimeoutSeconds(state.radio)
        ?.takeIf { state.connected && !state.txTimeoutReadFailed && state.txTimeoutSupported == true }
    val label = when {
        current != null -> "$current 秒"
        state.txTimeoutSupported == false -> "需升级固件"
        !state.connected -> "等待连接"
        retryable -> "读取失败 · 重试"
        else -> "读取中…"
    }
    fun retry() {
        manager.action(profile.id) { if (api.deviceId == device) readTxTimeout() }
    }
    var open by remember(device) { mutableStateOf(false) }
    SettingRow("发射超时", label, enabled=current != null || retryable) {
        if (retryable) retry() else open = true
    }
    if (!open) return

    val draft = drafts[device]?.takeUnless { it == current?.toString() }
    val text = draft ?: current?.toString().orEmpty()
    val forwarding = state.status.optBoolean("tx_timeout_busy") ||
        state.status.optJSONObject("bridge")?.value("rx_open") in listOf("1", "true")
    val transmitting = state.pttPending ||
        RadioStateRules.activity(state.connected,state.ptt,state.status,state.activity.sample) == RadioActivity.TRANSMIT
    val maintenance = state.maintenance || state.status.optBoolean("tx_timeout_maintenance") || state.deviceMaintenance.optBoolean("reboot_pending") ||
        state.deviceMaintenance.optString("busy") == "writing"
    var working by remember(device) { mutableStateOf(false) }
    var localError by remember(device) { mutableStateOf("") }
    val saving = working || state.txTimeoutSaving
    val editable = current != null && device.isNotBlank() && !saving
    val focusManager = LocalFocusManager.current

    fun save() {
        if (!editable || draft == null || transmitting || forwarding || maintenance || working) return
        localError = RadioUiRules.parameterError("tx_timeout_s", draft).orEmpty()
        if (localError.isNotEmpty()) return
        val seconds = draft.toInt()
        focusManager.clearFocus()
        working = true
        val job = manager.action(profile.id) {
            if (api.deviceId == device && saveTxTimeout(seconds)) {
                withContext(Dispatchers.Main) {
                    if (drafts[device] == draft) drafts.remove(device)
                    open = false
                }
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
        draft != null && !state.txTimeoutResult.startsWith("已保存") -> state.txTimeoutResult
        else -> ""
    }
    val status = listOfNotNull(feedback.takeIf { it.isNotEmpty() },
        current?.let { "未保存 · 当前生效 $it 秒" }?.takeIf { draft != null }).joinToString(" · ")

    ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true), onDismissRequest={ if (!saving) open=false }) {
        Column(Modifier.fillMaxWidth().padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
            .imePadding().verticalScroll(rememberScrollState()).padding(horizontal=24.dp).padding(bottom=20.dp),
            verticalArrangement=Arrangement.spacedBy(10.dp)) {
            Text("发射超时", style=MaterialTheme.typography.headlineSmall)
            OutlinedTextField(text, { value ->
                if (value == current?.toString()) drafts.remove(device) else drafts[device]=value
                localError=""
            }, enabled=editable, label={ Text("秒数") }, suffix={ Text("秒") }, singleLine=true,
                isError=localError.isNotEmpty(), modifier=Modifier.fillMaxWidth(),
                keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number, imeAction=ImeAction.Done),
                keyboardActions=KeyboardActions(onDone={ focusManager.clearFocus() }))
            Text("5–600 秒整数，默认 180 秒", color=secondary, style=MaterialTheme.typography.bodySmall)
            if (status.isNotBlank()) {
                Text(status, color=if(localError.isBlank())secondary else red, style=MaterialTheme.typography.bodySmall,
                    modifier=Modifier.semantics { liveRegion=LiveRegionMode.Polite })
            }
            if (retryable) TextButton(onClick=::retry) { Text("重试读取") }
            Row(Modifier.fillMaxWidth(), horizontalArrangement=Arrangement.spacedBy(8.dp, Alignment.End)) {
                TextButton(onClick={ drafts.remove(device); open=false }, enabled=!saving, modifier=Modifier.heightIn(min=48.dp)) {
                    Text("取消")
                }
                Button(onClick=::save, enabled=editable && draft != null && !transmitting && !forwarding && !maintenance,
                    modifier=Modifier.heightIn(min=48.dp)) { Text("保存") }
            }
        }
    }
}
