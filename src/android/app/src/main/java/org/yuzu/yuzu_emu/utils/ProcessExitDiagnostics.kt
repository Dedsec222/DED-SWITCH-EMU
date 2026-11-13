// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.utils

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import java.io.File
import org.yuzu.yuzu_emu.BuildConfig

object ProcessExitDiagnostics {
    fun record(context: Context) {
        Log.info("[BuildIdentity] version=${BuildConfig.VERSION_NAME} code=${BuildConfig.VERSION_CODE}")
        Thread({
            runCatching {
                val manager = context.getSystemService(ActivityManager::class.java)
                val exits = manager.getHistoricalProcessExitReasons(context.packageName, 0, 16)
                exits.take(3).forEach { exit ->
                    val reason = when (exit.reason) {
                        ApplicationExitInfo.REASON_CRASH_NATIVE -> "native crash"
                        ApplicationExitInfo.REASON_CRASH -> "Java crash"
                        ApplicationExitInfo.REASON_ANR -> "ANR"
                        ApplicationExitInfo.REASON_LOW_MEMORY -> "low memory"
                        ApplicationExitInfo.REASON_SIGNALED -> "signal"
                        ApplicationExitInfo.REASON_USER_REQUESTED -> "user request / force stop"
                        else -> "other"
                    }
                    Log.warning("[PreviousProcessExit] time=${exit.timestamp} reason=$reason " +
                        "code=${exit.reason} status=${exit.status} rssKiB=${exit.rss} " +
                        "pssKiB=${exit.pss} description=${exit.description}")
                }
                exits.firstOrNull { it.reason == ApplicationExitInfo.REASON_CRASH_NATIVE }?.let { exit ->
                    runCatching {
                        val summary = exit.traceInputStream?.use { input ->
                            val bytes = input.readNBytes(8 * 1024 * 1024 + 1)
                            NativeTombstone.summarize(bytes)
                        } ?: listOf("system trace unavailable")
                        val lines = summary.map { "[PreviousNativeTombstone] time=${exit.timestamp} $it" }
                        lines.forEach { Log.warning(it) }
                        val oldLog = File(context.getExternalFilesDir(null), "log/eden_log.txt.old.txt")
                        if (oldLog.isFile) oldLog.appendText("\n" + lines.joinToString("\n") + "\n")
                    }.onFailure {
                        Log.warning("[PreviousNativeTombstone] unavailable: ${it.javaClass.simpleName}")
                    }
                }
            }.onFailure { Log.warning("[PreviousProcessExit] unavailable: ${it.javaClass.simpleName}") }
        }, "ProcessExitDiagnostics").start()
    }
}
