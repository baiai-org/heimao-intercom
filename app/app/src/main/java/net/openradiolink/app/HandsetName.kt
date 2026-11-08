package net.openradiolink.app

import java.text.Normalizer

object HandsetName {
    fun normalized(input:String):String {
        val name=Normalizer.normalize(input.trim(),Normalizer.Form.NFC)
        require(name.codePointCount(0,name.length)<=24 && name.toByteArray(Charsets.UTF_8).size<=96) { "手台名称最多 24 字" }
        var i=0
        while(i<name.length) {
            val cp=name.codePointAt(i)
            require(cp !in 0..31 && cp !in 127..159 && cp !in 0xd800..0xdfff) { "名称不能包含控制字符或无效字符" }
            i+=Character.charCount(cp)
        }
        return name
    }
    fun discovery(input:String,id:String):String = runCatching { normalized(input) }.getOrDefault("").ifBlank { Brand.discoveryName(id) }
}
