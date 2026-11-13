// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.utils

// Bounded reader for debuggerd/proto/tombstone.proto. Skip memory dumps and other threads.
internal object NativeTombstone {
    private data class Field(val number: Int, val value: Long = 0, val bytes: ByteArray? = null)

    private fun fields(data: ByteArray, vararg wanted: Int): List<Field> {
        var position = 0
        var count = 0
        fun varint(): Long {
            var result = 0L
            for (shift in 0..63 step 7) {
                require(position < data.size) { "truncated varint" }
                val byte = data[position++].toInt() and 255
                require(shift < 63 || byte <= 1) { "varint overflow" }
                result = result or ((byte and 127).toLong() shl shift)
                if (byte and 128 == 0) return result
            }
            error("varint overflow")
        }
        val result = mutableListOf<Field>()
        while (position < data.size) {
            require(++count <= 65536) { "too many fields" }
            val tag = varint()
            val fieldNumber = tag ushr 3
            require(fieldNumber > 0 && fieldNumber <= 0x1fffffff) { "invalid field" }
            val number = fieldNumber.toInt()
            val keep = number in wanted
            when ((tag and 7).toInt()) {
                0 -> {
                    val value = varint()
                    if (keep) result += Field(number, value)
                }
                1, 5 -> {
                    val length = if (tag and 7 == 1L) 8 else 4
                    require(length <= data.size - position) { "truncated fixed field" }
                    position += length
                }
                2 -> {
                    val length = varint()
                    require(length >= 0 && length <= (data.size - position).toLong()) {
                        "truncated bytes"
                    }
                    val end = position + length.toInt()
                    if (keep) result += Field(number, bytes = data.copyOfRange(position, end))
                    position = end
                }
                else -> error("unsupported wire type")
            }
        }
        return result
    }

    private fun List<Field>.value(number: Int) = firstOrNull { it.number == number }?.value ?: 0L
    private fun List<Field>.text(number: Int) = firstOrNull { it.number == number }?.bytes
        ?.take(1024)?.toByteArray()?.toString(Charsets.UTF_8)
        ?.replace(Regex("[\\r\\n\\u0000]"), " ") ?: ""
    private fun hex(value: Long) = java.lang.Long.toUnsignedString(value, 16)

    fun summarize(data: ByteArray): List<String> {
        require(data.size <= 8 * 1024 * 1024) { "tombstone exceeds limit" }
        val root = fields(data, 6, 10, 14, 16)
        val tid = root.value(6)
        val signal = root.firstOrNull { it.number == 10 }?.bytes?.let { fields(it, 1, 2) }
        val lines = mutableListOf("tid=$tid signal=${signal?.text(2)} number=${signal?.value(1)} " +
            "abort=${root.text(14)}")
        for (entry in root.filter { it.number == 16 }) {
            val pair = fields(entry.bytes ?: continue, 1, 2)
            if (pair.value(1) != tid) continue
            val thread = fields(pair.firstOrNull { it.number == 2 }?.bytes ?: continue, 2, 4)
            lines += "thread=${thread.text(2)}"
            thread.filter { it.number == 4 }.take(64).forEachIndexed { index, field ->
                val frame = fields(field.bytes ?: return@forEachIndexed, 1, 4, 5, 6, 8)
                lines += "#$index relpc=0x${hex(frame.value(1))} file=${frame.text(6)} " +
                    "function=${frame.text(4)}+0x${hex(frame.value(5))} buildId=${frame.text(8)}"
            }
            break
        }
        return lines
    }
}
