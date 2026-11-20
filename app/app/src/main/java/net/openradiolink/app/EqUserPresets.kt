package net.openradiolink.app

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

internal data class EqCurves(val spk: List<Int>, val mic: List<Int>) {
    fun editable(): Boolean = EqPresets.valid(spk) && EqPresets.valid(mic)

    fun putInto(json: JSONObject, prefix: String = ""): JSONObject = json
        .put(prefix + "spk", JSONArray(spk))
        .put(prefix + "mic", JSONArray(mic))

    companion object {
        fun parse(json: JSONObject, prefix: String = "", ceiling: Int = EqPresets.MAX): EqCurves {
            fun bands(dir: String): List<Int> {
                val array = json.getJSONArray(prefix + dir)
                require(array.length() == EqPresets.bands.size) { "EQ 需要 6 段" }
                return List(array.length()) { index ->
                    val n = array.get(index) as? Number ?: error("EQ 数值无效")
                    val band = n.toInt()
                    require(n.toDouble() == band.toDouble() && band in EqPresets.MIN..ceiling) { "EQ 数值超出范围" }
                    band
                }
            }
            return EqCurves(bands("spk"), bands("mic"))
        }
    }
}

internal data class EqUserSlot(val name: String, val curves: EqCurves) {
    companion object {
        fun nameValid(name: String): Boolean = name.isNotBlank() &&
            name.codePointCount(0, name.length) <= 24 && name.toByteArray(Charsets.UTF_8).size <= 96 &&
            name.codePoints().allMatch { it !in 0..0x1f && it !in 0x7f..0x9f && it !in 0xd800..0xdfff }
    }
}

internal data class EqSlots(
    val device: String, val boot: String, val revision: Long,
    val slots: List<EqUserSlot?>, val active: EqCurves
) {
    companion object {
        const val COUNT = 3
        fun parse(json: JSONObject, device: String): EqSlots {
            val boot = json.getString("boot_id")
            val revision = json.getLong("revision")
            val array = json.getJSONArray("slots")
            require(device.isNotEmpty() && json.optString("device_id") == device && boot.matches(Regex("[0-9a-fA-F]{8}")) &&
                revision in 0..0xFFFFFFFFL && array.length() == COUNT) { "EQ 保存栏回读无效" }
            val slots = List(COUNT) { index ->
                if (array.isNull(index)) null else {
                    val item = array.getJSONObject(index)
                    val name = item.getString("name")
                    require(EqUserSlot.nameValid(name)) { "EQ 预设名称无效" }
                    EqUserSlot(name, EqCurves.parse(item))
                }
            }
            return EqSlots(device, boot, revision, slots, EqCurves.parse(json, "active_", ceiling = 12))
        }
    }
}

internal enum class EqSlotOp(val key: String, val feedback: String) {
    SAVE("save", "已保存到槽位"),
    RENAME("rename", "已重命名"),
    DELETE("delete", "已删除"),
    APPLY("apply", "已应用接收和发射 EQ")
}

internal data class EqSlotWrite(
    val op: EqSlotOp, val slot: Int, val base: EqSlots,
    val name: String = "", val curves: EqCurves? = null
) {
    fun body(): JSONObject {
        require(slot in 1..EqSlots.COUNT) { "EQ 槽位无效" }
        val data = JSONObject().put("op", op.key).put("slot", slot)
            .put("revision", base.revision).put("boot_id", base.boot)
        return when (op) {
            EqSlotOp.SAVE -> {
                require(EqUserSlot.nameValid(name) && curves != null && curves.editable()) { "预设名称或曲线无效" }
                curves.putInto(data.put("name", name))
            }
            EqSlotOp.RENAME -> {
                require(EqUserSlot.nameValid(name)) { "预设名称无效" }
                data.put("name", name)
            }
            EqSlotOp.DELETE -> data
            EqSlotOp.APPLY -> base.active.putInto(data, "expected_")
        }
    }

    fun confirmed(result: EqSlots): Boolean {
        if (result.device != base.device || result.boot != base.boot) return false
        val old = base.slots[slot - 1]
        val item = result.slots[slot - 1]
        if (op == EqSlotOp.APPLY) return result.revision == base.revision && old?.curves == result.active
        if (result.revision != base.revision + 1) return false
        return when (op) {
            EqSlotOp.DELETE -> item == null
            EqSlotOp.SAVE -> item != null && item.name == name && item.curves == curves
            EqSlotOp.RENAME -> item != null && item.name == name && item.curves == old?.curves
            EqSlotOp.APPLY -> false
        }
    }
}

internal class EqSlotUnconfirmed(message: String, val latest: EqSlots?, cause: Throwable? = null) :
    IOException(message, cause)
