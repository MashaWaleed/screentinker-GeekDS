package com.remotedisplay.player.player

import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * Canonical per-playlist-item schedule evaluator (#74 dayparting + #75 expiry + play window) -
 * Kotlin port of server/lib/schedule-eval.js.
 *
 * CONTRACT: shared/schedule-vectors.json. This must agree with the JS evaluator
 * (server/web/Tizen) on every vector. If it disagrees with a vector, this is wrong.
 *
 * Time model: instants are UTC; blocks and play windows are LOCAL wall-clock rules
 * interpreted in the device's effective IANA timezone (DST handled by java.time).
 * They are never converted to UTC.
 *
 * Block semantics:
 *  - within a block, day AND date AND time must all pass; blocks OR together
 *  - zero blocks = always on ("no schedule = always plays")
 *  - time window is [start, end): start inclusive, end exclusive ("24:00" = end of day)
 *  - start > end crosses midnight; the day/date test anchors to the day the window STARTED
 *
 * Play window (optional): local "YYYY-MM-DDTHH:MM" on each side, inclusive at the minute.
 * An INTERVAL, not a daypart. AND'd with blocks. Omitted = no extra gate.
 *
 * FAILS OPEN: any error (bad timezone, malformed block, bad stamp) returns true so the item
 * PLAYS. A blank screen is worse than an over-running promo.
 */
object ScheduleEval {

    data class Block(
        val days: Set<Int>,        // 0=Sun .. 6=Sat
        val start: String,         // "HH:MM"
        val end: String,           // "HH:MM" or "24:00"
        val startDate: String?,    // "YYYY-MM-DD" or null = no lower bound
        val endDate: String?       // "YYYY-MM-DD" or null = no upper bound
    )

    data class Window(
        val playFrom: String?,     // "YYYY-MM-DDTHH:MM" or null
        val playUntil: String?
    )

    private val STAMP = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm")
    private val STAMP_RE = Regex("""^\d{4}-\d{2}-\d{2}T([01]\d|2[0-3]):[0-5]\d$""")

    fun windowOf(playFrom: String?, playUntil: String?): Window? {
        val from = playFrom?.ifBlank { null }
        val until = playUntil?.ifBlank { null }
        if (from == null && until == null) return null
        return Window(from, until)
    }

    @JvmOverloads
    fun isItemActiveNow(
        blocks: List<Block>?,
        utcNowMs: Long,
        ianaTz: String?,
        window: Window? = null
    ): Boolean {
        return try {
            val zone = if (ianaTz.isNullOrBlank()) ZoneId.systemDefault() else ZoneId.of(ianaTz)
            val zdt = Instant.ofEpochMilli(utcNowMs).atZone(zone)
            val dow = zdt.dayOfWeek.value % 7          // java Mon=1..Sun=7 -> Sun=0..Sat=6
            val nowMin = zdt.hour * 60 + zdt.minute
            val date = zdt.toLocalDate()
            if (!intervalOk(window, zdt.toLocalDateTime().withSecond(0).withNano(0))) return false
            if (blocks.isNullOrEmpty()) return true
            blocks.any { blockMatches(it, dow, nowMin, date) }
        } catch (e: Throwable) {
            // Throwable, not Exception. A missing java.time on an old API level surfaces as
            // NoClassDefFoundError — an Error — which sailed straight through a catch(Exception)
            // and turned this "fail open, a blank screen is worse than an over-running promo"
            // contract into its exact opposite: nothing played at all. Desugaring (see
            // build.gradle.kts) is the real fix; this makes the guard mean what it says.
            true // fail open
        }
    }

    private fun intervalOk(window: Window?, now: LocalDateTime): Boolean {
        if (window == null) return true
        val from = window.playFrom
        val until = window.playUntil
        if (from == null && until == null) return true
        if (from != null && !STAMP_RE.matches(from)) throw IllegalArgumentException("bad play_from")
        if (until != null && !STAMP_RE.matches(until)) throw IllegalArgumentException("bad play_until")
        if (from != null && now.isBefore(LocalDateTime.parse(from, STAMP))) return false
        if (until != null && now.isAfter(LocalDateTime.parse(until, STAMP))) return false
        return true
    }

    private fun hm(s: String): Int { val p = s.split(":"); return p[0].toInt() * 60 + p[1].toInt() } // "24:00" -> 1440

    private fun dayOk(dow: Int, days: Set<Int>): Boolean = days.contains(dow)

    private fun dateOk(date: LocalDate, startDate: String?, endDate: String?): Boolean {
        if (startDate != null && date.isBefore(LocalDate.parse(startDate))) return false
        if (endDate != null && date.isAfter(LocalDate.parse(endDate))) return false   // inclusive
        return true
    }

    private fun blockMatches(b: Block, dow: Int, nowMin: Int, date: LocalDate): Boolean {
        val s = hm(b.start); val e = hm(b.end)
        if (s <= e) {
            // same-day window [s, e), anchored to today
            if (nowMin < s || nowMin >= e) return false
            return dayOk(dow, b.days) && dateOk(date, b.startDate, b.endDate)
        }
        // overnight wrap
        if (nowMin >= s) {
            // before-midnight portion: anchor = today
            return dayOk(dow, b.days) && dateOk(date, b.startDate, b.endDate)
        }
        if (nowMin < e) {
            // after-midnight portion: anchor = the day it started = yesterday
            val y = date.minusDays(1)
            return dayOk((dow + 6) % 7, b.days) && dateOk(y, b.startDate, b.endDate)
        }
        return false
    }
}
