package net.openradiolink.app

/**
 * Rules for the management-panel domain: the <name> in https://<name>.local.
 * Android-free so it can be unit-tested; mirrors the firmware's valid_host().
 */
internal object PanelDomain {
    const val MAX=63
    private val RE=Regex("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")

    fun normalize(raw:String)=raw.trim().lowercase()
    fun valid(name:String)=name.isNotEmpty() && name.length<=MAX && RE.matches(name)
    fun address(name:String)=if(valid(name)) "https://$name.local" else ""
    fun error(name:String):String=when {
        name.isEmpty()->"请填写管理面板访问域名"
        name.length>MAX->"域名最长 $MAX 个字符"
        !name.first().isAsciiDigitOrLetter()->"首字符必须是字母或数字"
        !name.last().isAsciiDigitOrLetter()->"末字符必须是字母或数字"
        else->"只能用字母、数字和连字符"
    }
    private fun Char.isAsciiDigitOrLetter()=this in 'a'..'z' || this in '0'..'9'
}
