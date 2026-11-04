package io.polypulse.observatory.data

import android.util.Base64
import androidx.core.net.toUri
import io.polypulse.observatory.CockpitTab
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

data class CockpitCredentials(
    val serverUrl: String,
    val username: String,
    val password: String,
    val localNetworkPermissionRequested: Boolean = false,
)

data class SectionResult(
    val payloads: Map<String, JSONObject> = emptyMap(),
    val errors: Map<String, String> = emptyMap(),
)

class CockpitHttpException(val status: Int, message: String) : Exception(message)

object CockpitApi {
    private const val CONNECT_TIMEOUT_MS = 8_000
    private const val READ_TIMEOUT_MS = 12_000

    fun validateServerUrl(raw: String): String {
        val cleaned = raw.trim().trimEnd('/')
        val uri = runCatching { cleaned.toUri() }.getOrNull()
            ?: throw IllegalArgumentException("Enter a valid cockpit URL.")
        val host = uri.host?.lowercase()?.trim('[', ']')
        if (host.isNullOrBlank()) throw IllegalArgumentException("Enter a valid cockpit URL with a host name.")
        if (uri.encodedAuthority?.contains('@') == true) {
            throw IllegalArgumentException("Enter the cockpit URL without embedded credentials.")
        }
        if (uri.encodedQuery != null || uri.encodedFragment != null) {
            throw IllegalArgumentException("Enter the cockpit base URL without a query or fragment.")
        }
        if (uri.scheme.equals("https", ignoreCase = true)) return cleaned
        val localHosts = setOf("localhost", "127.0.0.1", "10.0.2.2", "::1")
        if (uri.scheme.equals("http", ignoreCase = true) && host in localHosts) return cleaned
        throw IllegalArgumentException("Use HTTPS for remote servers. HTTP is allowed only for Android emulator loopback.")
    }

    fun requiresLocalNetworkPermission(serverUrl: String): Boolean {
        val host = runCatching { serverUrl.toUri().host?.lowercase()?.trim('[', ']') }.getOrNull()
            ?: return false
        if (host in setOf("localhost", "::1") || host.endsWith(".localhost") ||
            listOf(".local", ".lan", ".home", ".internal").any(host::endsWith)
        ) return true

        if (':' in host) return host.startsWith("fc") || host.startsWith("fd") || host.startsWith("fe80")

        val octets = host.split('.').mapNotNull(String::toIntOrNull)
        if (octets.size == 4 && octets.all { it in 0..255 }) {
            val (first, second) = octets
            return first == 0 || first == 10 || first == 127 ||
                (first == 169 && second == 254) ||
                (first == 172 && second in 16..31) ||
                (first == 192 && second == 168) ||
                (first == 100 && second in 64..127)
        }

        // A single-label host is usually resolved by a private DNS suffix or local search domain.
        return '.' !in host
    }

    suspend fun connect(profile: CockpitCredentials): JSONObject = get(profile, "/api/whoami")

    suspend fun get(profile: CockpitCredentials, path: String): JSONObject = withContext(Dispatchers.IO) {
        val connection = open(profile, path)
        try {
            val status = connection.responseCode
            val body = readBody(connection, status)
            if (status !in 200..299) {
                val message = runCatching { JSONObject(body).optString("error") }.getOrNull()
                    ?.takeIf { it.isNotBlank() }
                    ?: when {
                        status == 401 -> "Sign-in was rejected. Check your cockpit username and password."
                        status in 300..399 -> "The cockpit redirected this request. Enter its final HTTPS URL."
                        else -> "Cockpit returned HTTP $status."
                    }
                throw CockpitHttpException(status, message)
            }
            val payload = JSONObject(body)
            if (payload.optBoolean("ok", true).not() && payload.has("error")) {
                throw CockpitHttpException(status, payload.optString("error"))
            }
            payload
        } finally {
            connection.disconnect()
        }
    }

    suspend fun loadSection(
        profile: CockpitCredentials,
        tab: CockpitTab,
        instance: String?,
    ): SectionResult = coroutineScope {
        val requests = when (tab) {
            CockpitTab.OVERVIEW -> listOf(
                Endpoint("activity", scoped("/api/activity", instance)),
                Endpoint("pnl", "/api/pnl_today"),
                Endpoint("prices", "/api/prices"),
            )
            CockpitTab.POSITIONS -> listOf(Endpoint("activity", scoped("/api/activity", instance)))
            CockpitTab.TRADE_HISTORY -> listOf(
                Endpoint("history", query("/api/settlement_history", "limit" to "60", "instance" to instance.orEmpty())),
            )
            CockpitTab.EXECUTION -> listOf(Endpoint("execution", scoped("/api/execution", instance)))
            CockpitTab.MARKETS -> listOf(Endpoint("markets", "/api/markets?limit=60"))
            CockpitTab.PREDICTIONS -> listOf(Endpoint("predictions", "/api/manual_predictions"))
            CockpitTab.BOT_FLEET -> listOf(
                Endpoint("fleet", "/api/fleet"),
                Endpoint("pnl", "/api/pnl_by_instance"),
            )
            CockpitTab.WINDOW_BOTS -> listOf(Endpoint("window_bots", "/api/window_bots"))
            CockpitTab.STRATEGIES -> buildList {
                add(Endpoint("fleet", "/api/fleet"))
                if (instance.isNullOrBlank()) add(Endpoint("strategies", "/api/supervisor_feed?limit=30"))
                else add(Endpoint("strategies", query("/api/strategy_history", "instance" to instance, "limit" to "40")))
            }
            CockpitTab.ENGINE_LAB -> listOf(
                Endpoint("predictor", query("/api/predictor", "instance" to (instance ?: "predictor"), "limit" to "100")),
                Endpoint("trajectory", "/api/predictor_trajectory?limit=100"),
                Endpoint("signals", "/api/signal_history?limit=30"),
                Endpoint("performance", "/api/perf_stats"),
            )
            CockpitTab.ACTIVITY_LOGS -> listOf(
                Endpoint("logs", query("/api/log_tail", "lines" to "120", "instance" to instance.orEmpty())),
                Endpoint("supervisor_feed", "/api/supervisor_feed?limit=20"),
                Endpoint("supervisor_log", "/api/supervisor_log?limit=20"),
            )
        }
        val results = requests.map { endpoint ->
            async {
                try {
                    ResultRow(endpoint.key, get(profile, endpoint.path), null)
                } catch (error: Throwable) {
                    if (error is CancellationException) throw error
                    ResultRow(endpoint.key, null, readableError(error))
                }
            }
        }.awaitAll()
        SectionResult(
            payloads = results.mapNotNull { row -> row.payload?.let { row.key to it } }.toMap(),
            errors = results.mapNotNull { row -> row.error?.let { row.key to it } }.toMap(),
        )
    }

    suspend fun stream(
        profile: CockpitCredentials,
        onFrame: suspend (JSONObject) -> Unit,
    ) = withContext(Dispatchers.IO) {
        val connection = open(profile, "/api/stream").apply {
            readTimeout = 10_000
            setRequestProperty("Accept", "text/event-stream")
            setRequestProperty("Cache-Control", "no-cache")
        }
        try {
            val status = connection.responseCode
            if (status !in 200..299) {
                val body = readBody(connection, status)
                val message = when {
                    status == 401 -> "Live stream authentication expired."
                    status in 300..399 -> "The cockpit redirected the live stream. Enter its final HTTPS URL."
                    else -> "Live stream returned HTTP $status: $body"
                }
                throw CockpitHttpException(status, message)
            }
            connection.inputStream.bufferedReader(StandardCharsets.UTF_8).use { reader ->
                while (currentCoroutineContext().isActive) {
                    val line = reader.readLine() ?: break
                    if (!line.startsWith("data:")) continue
                    val encoded = line.removePrefix("data:").trim()
                    val frame = runCatching { JSONObject(encoded) }.getOrNull() ?: continue
                    withContext(Dispatchers.Main.immediate) { onFrame(frame) }
                }
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun open(profile: CockpitCredentials, path: String): HttpURLConnection {
        val base = validateServerUrl(profile.serverUrl)
        val url = URL(base + path)
        val token = Base64.encodeToString(
            "${profile.username}:${profile.password}".toByteArray(StandardCharsets.UTF_8),
            Base64.NO_WRAP,
        )
        return (url.openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            // Basic auth must never be forwarded to a redirect target.
            instanceFollowRedirects = false
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            useCaches = false
            setRequestProperty("Authorization", "Basic $token")
            setRequestProperty("Accept", "application/json")
            setRequestProperty("Cache-Control", "no-cache")
        }
    }

    private fun readBody(connection: HttpURLConnection, status: Int): String {
        val stream = if (status in 200..299) connection.inputStream else connection.errorStream
        return stream?.bufferedReader(StandardCharsets.UTF_8)?.use { it.readText() }.orEmpty()
    }

    private fun scoped(path: String, instance: String?): String =
        if (instance.isNullOrBlank()) path else query(path, "instance" to instance)

    private fun query(path: String, vararg params: Pair<String, String>): String {
        val values = params.filter { it.second.isNotBlank() }
        if (values.isEmpty()) return path
        return path + "?" + values.joinToString("&") {
            "${encode(it.first)}=${encode(it.second)}"
        }
    }

    private fun encode(value: String): String = URLEncoder.encode(value, StandardCharsets.UTF_8.name())

    private fun readableError(error: Throwable): String = when (error) {
        is CockpitHttpException -> error.message ?: "Cockpit request failed."
        else -> error.message?.takeIf { it.isNotBlank() } ?: "Could not reach the cockpit server."
    }

    private data class Endpoint(val key: String, val path: String)
    private data class ResultRow(val key: String, val payload: JSONObject?, val error: String?)
}
