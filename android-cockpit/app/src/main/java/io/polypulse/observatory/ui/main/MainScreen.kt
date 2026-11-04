package io.polypulse.observatory

import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke as DrawStroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.core.content.ContextCompat
import io.polypulse.observatory.data.CockpitApi
import io.polypulse.observatory.data.CockpitCredentials
import io.polypulse.observatory.data.CockpitHttpException
import io.polypulse.observatory.data.CredentialStore
import io.polypulse.observatory.data.SectionResult
import io.polypulse.observatory.theme.Aqua
import io.polypulse.observatory.theme.Coral
import io.polypulse.observatory.theme.Faint
import io.polypulse.observatory.theme.Gold
import io.polypulse.observatory.theme.Ink
import io.polypulse.observatory.theme.Mint
import io.polypulse.observatory.theme.MintDim
import io.polypulse.observatory.theme.Muted
import io.polypulse.observatory.theme.Night
import io.polypulse.observatory.theme.Stroke
import io.polypulse.observatory.theme.Surface
import io.polypulse.observatory.theme.SurfaceInset
import io.polypulse.observatory.theme.SurfaceRaised
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

private const val ACCESS_LOCAL_NETWORK_PERMISSION = "android.permission.ACCESS_LOCAL_NETWORK"

enum class CockpitTab(val title: String, val refreshMs: Long) {
    OVERVIEW("Overview", 8_000),
    POSITIONS("Positions", 7_000),
    TRADE_HISTORY("Trade history", 15_000),
    EXECUTION("Execution", 3_000),
    MARKETS("Markets", 4_000),
    PREDICTIONS("Predictions", 5_000),
    BOT_FLEET("Bot fleet", 8_000),
    WINDOW_BOTS("Window bots", 6_000),
    STRATEGIES("Strategies", 10_000),
    ENGINE_LAB("Engine lab", 10_000),
    ACTIVITY_LOGS("Activity logs", 6_000),
}

private data class ObservatoryState(
    val live: JSONObject? = null,
    val liveError: String? = null,
    val stream: JSONObject? = null,
    val streamConnected: Boolean = false,
    val lastSyncMs: Long = 0,
    val pages: Map<CockpitTab, SectionResult> = emptyMap(),
)

private data class PendingLocalNetworkRequest(
    val host: String,
    val proceed: () -> Unit,
    val deny: () -> Unit,
)

@Composable
fun CockpitApp() {
    val context = LocalContext.current
    val store = remember { CredentialStore(context.applicationContext) }
    val scope = rememberCoroutineScope()
    var credentials by remember { mutableStateOf<CockpitCredentials?>(null) }
    var profile by remember { mutableStateOf<JSONObject?>(null) }
    var loadingSaved by remember { mutableStateOf(true) }
    var loginError by remember { mutableStateOf<String?>(null) }
    var localPermissionRequest by remember { mutableStateOf<PendingLocalNetworkRequest?>(null) }
    var showLocalPermissionRationale by remember { mutableStateOf(false) }

    val connectToServer: (CockpitCredentials, (String?) -> Unit) -> Unit = { candidate, onComplete ->
        scope.launch {
            try {
                val who = CockpitApi.connect(candidate)
                withContext(Dispatchers.IO) { store.save(candidate) }
                credentials = candidate
                profile = who
                loginError = null
                onComplete(null)
            } catch (error: Throwable) {
                if (error is CancellationException) throw error
                onComplete(readable(error))
            }
        }
    }

    val requestConnection: (CockpitCredentials, Boolean, (String?) -> Unit) -> Unit = { candidate, forceLocalPermission, onComplete ->
        val connection = candidate.copy(
            localNetworkPermissionRequested = candidate.localNetworkPermissionRequested || forceLocalPermission,
        )
        val requiresPermission = Build.VERSION.SDK_INT >= 37 &&
            (connection.localNetworkPermissionRequested || CockpitApi.requiresLocalNetworkPermission(connection.serverUrl)) &&
            ContextCompat.checkSelfPermission(context, ACCESS_LOCAL_NETWORK_PERMISSION) != PackageManager.PERMISSION_GRANTED
        if (requiresPermission) {
            val host = runCatching { URI(connection.serverUrl).host }.getOrNull().orEmpty()
            localPermissionRequest = PendingLocalNetworkRequest(
                host = host,
                proceed = { connectToServer(connection, onComplete) },
                deny = { onComplete("Local network access is required to connect to this cockpit. You can allow it in Android Settings and try again.") },
            )
            showLocalPermissionRationale = true
        } else {
            connectToServer(connection, onComplete)
        }
    }

    val localPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val pending = localPermissionRequest
        localPermissionRequest = null
        showLocalPermissionRationale = false
        if (granted) pending?.proceed?.invoke() else pending?.deny?.invoke()
    }

    LaunchedEffect(Unit) {
        val saved = withContext(Dispatchers.IO) { store.load() }
        if (saved != null) {
            requestConnection(saved, saved.localNetworkPermissionRequested) { error ->
                if (error != null) {
                    loginError = error
                    if (error.startsWith("Sign-in was rejected")) {
                        scope.launch { withContext(Dispatchers.IO) { store.clear() } }
                        loginError = "Your saved sign-in expired. Please sign in again."
                    }
                }
            }
        }
        loadingSaved = false
    }

    if (loadingSaved) {
        SplashScreen()
    } else if (credentials == null) {
        SignInScreen(
            initialError = loginError,
            onConnect = { server, username, password, privateNetworkServer ->
                try {
                    loginError = null
                    val cleaned = CockpitApi.validateServerUrl(server)
                    val candidate = CockpitCredentials(cleaned, username.trim(), password)
                    requestConnection(candidate, privateNetworkServer) { error -> loginError = error }
                } catch (error: Throwable) {
                    loginError = readable(error)
                }
            },
        )
    } else {
        CockpitHome(
            credentials = credentials!!,
            profile = profile,
            onConnectToServer = requestConnection,
            onLogout = {
                scope.launch {
                    withContext(Dispatchers.IO) { store.clear() }
                    credentials = null
                    profile = null
                    loginError = null
                }
            },
            onSessionExpired = {
                scope.launch {
                    withContext(Dispatchers.IO) { store.clear() }
                    credentials = null
                    profile = null
                    loginError = "The cockpit rejected this session. Sign in again."
                }
            },
        )
    }

    val pending = localPermissionRequest
    if (showLocalPermissionRationale && pending != null) {
        AlertDialog(
            onDismissRequest = {
                localPermissionRequest = null
                showLocalPermissionRationale = false
                pending.deny()
            },
            containerColor = SurfaceRaised,
            title = { Text("Allow local cockpit access?", color = Ink, fontWeight = FontWeight.Bold) },
            text = {
                Text(
                    "Android protects private networks. PolySparQ Observatory needs local network access to connect to ${pending.host} and stream your cockpit data.",
                    color = Muted,
                    fontSize = 12.sp,
                    lineHeight = 18.sp,
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    localPermissionLauncher.launch(ACCESS_LOCAL_NETWORK_PERMISSION)
                }) { Text("Continue", color = Mint, fontWeight = FontWeight.Bold) }
            },
            dismissButton = {
                TextButton(onClick = {
                    localPermissionRequest = null
                    showLocalPermissionRationale = false
                    pending.deny()
                }) { Text("Not now", color = Muted) }
            },
        )
    }
}

@Composable
private fun SplashScreen() {
    Box(
        Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF111C21), Night))),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            BrandMark()
            Spacer(Modifier.height(22.dp))
            Text("SECURE COCKPIT LINK", color = Muted, fontSize = 12.sp, letterSpacing = 2.sp)
        }
    }
}

@Composable
private fun SignInScreen(
    initialError: String?,
    onConnect: (String, String, String, Boolean) -> Unit,
) {
    var server by rememberSaveable { mutableStateOf("") }
    var username by rememberSaveable { mutableStateOf("") }
    var password by rememberSaveable { mutableStateOf("") }
    var privateNetworkServer by rememberSaveable { mutableStateOf(false) }
    var connecting by remember { mutableStateOf(false) }
    var error by remember(initialError) { mutableStateOf(initialError) }

    Box(
        Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF10201F), Night)))
            .windowInsetsPadding(WindowInsets.statusBars).windowInsetsPadding(WindowInsets.navigationBars)
            .verticalScroll(rememberScrollState()).padding(horizontal = 22.dp, vertical = 34.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(28.dp)).background(Surface)
                .border(1.dp, Stroke, RoundedCornerShape(28.dp)).padding(24.dp),
        ) {
            BrandMark()
            Spacer(Modifier.height(30.dp))
            Text("Your trading floor.\nIn your pocket.", color = Ink, fontSize = 30.sp, lineHeight = 35.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(8.dp))
            Text("Connect to your PolySparQ++ cockpit to stream fleet, market, execution and research activity.", color = Muted, fontSize = 12.sp, lineHeight = 21.sp)
            Spacer(Modifier.height(25.dp))
            FieldLabel("COCKPIT SERVER")
            OutlinedTextField(
                value = server, onValueChange = { server = it; error = null },
                modifier = Modifier.fillMaxWidth(), singleLine = true,
                placeholder = { Text("https://your-cockpit.example", color = Faint) },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                colors = fieldColors(), shape = RoundedCornerShape(13.dp),
            )
            if (Build.VERSION.SDK_INT >= 37) {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 5.dp)) {
                    Checkbox(
                        checked = privateNetworkServer,
                        onCheckedChange = { privateNetworkServer = it },
                        colors = CheckboxDefaults.colors(checkedColor = Mint, uncheckedColor = Stroke, checkmarkColor = Night),
                    )
                    Column {
                        Text("Private-network cockpit", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                        Text("Request Android local-network access for internal hostnames", color = Faint, fontSize = 12.sp, lineHeight = 13.sp)
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            FieldLabel("USERNAME")
            OutlinedTextField(
                value = username, onValueChange = { username = it; error = null },
                modifier = Modifier.fillMaxWidth(), singleLine = true,
                placeholder = { Text("Cockpit username", color = Faint) },
                colors = fieldColors(), shape = RoundedCornerShape(13.dp),
            )
            Spacer(Modifier.height(12.dp))
            FieldLabel("PASSWORD")
            OutlinedTextField(
                value = password, onValueChange = { password = it; error = null },
                modifier = Modifier.fillMaxWidth(), singleLine = true,
                placeholder = { Text("Cockpit password", color = Faint) },
                visualTransformation = PasswordVisualTransformation(),
                colors = fieldColors(), shape = RoundedCornerShape(13.dp),
            )
            if (error != null) {
                Spacer(Modifier.height(10.dp))
                Text(error!!, color = Coral, fontSize = 12.sp, lineHeight = 17.sp)
            }
            Spacer(Modifier.height(20.dp))
            Button(
                onClick = {
                    if (server.isBlank() || username.isBlank() || password.isBlank()) {
                        error = "Enter the server URL, username and password to connect."
                    } else {
                        connecting = true
                        onConnect(server, username, password, privateNetworkServer)
                    }
                },
                modifier = Modifier.fillMaxWidth().height(54.dp),
                enabled = !connecting,
                shape = RoundedCornerShape(15.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Mint, contentColor = Night),
            ) {
                Text(if (connecting) "CONNECTING…" else "CONNECT SECURELY  →", fontWeight = FontWeight.Bold, letterSpacing = .6.sp)
            }
            LaunchedEffect(initialError, error) {
                if (initialError != null || error != null) connecting = false
            }
            Spacer(Modifier.height(15.dp))
            Text("Remote connections require HTTPS. Credentials are encrypted locally with Android Keystore.", color = Faint, fontSize = 12.sp, lineHeight = 15.sp)
        }
    }
}

@Composable
private fun CockpitHome(
    credentials: CockpitCredentials,
    profile: JSONObject?,
    onConnectToServer: (CockpitCredentials, Boolean, (String?) -> Unit) -> Unit,
    onLogout: () -> Unit,
    onSessionExpired: () -> Unit,
) {
    val lifecycle = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    var foreground by remember { mutableStateOf(lifecycle.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    var selectedTab by rememberSaveable { mutableStateOf(CockpitTab.OVERVIEW) }
    var selectedInstance by rememberSaveable { mutableStateOf("") }
    var predictionAsset by rememberSaveable { mutableStateOf("ALL") }
    var predictionHorizon by rememberSaveable { mutableStateOf(300) }
    var predictionWindow by rememberSaveable { mutableStateOf("CURRENT") }
    var expandedPredictionHistory by rememberSaveable { mutableStateOf("") }
    var predictionHistory by remember { mutableStateOf<Map<String, JSONObject>>(emptyMap()) }
    var predictionHistoryErrors by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    var predictionHistoryLoading by remember { mutableStateOf("") }
    var predictionHistoryRefresh by remember { mutableStateOf(0) }
    var showMoreSections by remember { mutableStateOf(false) }
    var state by remember { mutableStateOf(ObservatoryState()) }
    var showSettings by remember { mutableStateOf(false) }
    var settingsSaving by remember { mutableStateOf(false) }
    var settingsError by remember { mutableStateOf<String?>(null) }

    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START, Lifecycle.Event.ON_RESUME -> foreground = true
                Lifecycle.Event.ON_STOP, Lifecycle.Event.ON_PAUSE -> foreground = false
                else -> Unit
            }
        }
        lifecycle.lifecycle.addObserver(observer)
        onDispose { lifecycle.lifecycle.removeObserver(observer) }
    }

    LaunchedEffect(credentials, selectedInstance, foreground) {
        if (!foreground) return@LaunchedEffect
        val instanceQuery = if (selectedInstance.isBlank()) "" else "?instance=${java.net.URLEncoder.encode(selectedInstance, "UTF-8")}"
        while (isActive) {
            try {
                val live = CockpitApi.get(credentials, "/api/live_lite$instanceQuery")
                state = state.copy(live = live, liveError = null, lastSyncMs = System.currentTimeMillis())
            } catch (error: Throwable) {
                if (error is CancellationException) throw error
                if (error is CockpitHttpException && error.status == 401) {
                    onSessionExpired()
                    return@LaunchedEffect
                }
                state = state.copy(liveError = readable(error))
            }
            delay(2_500)
        }
    }

    LaunchedEffect(credentials, foreground) {
        if (!foreground) return@LaunchedEffect
        while (isActive) {
            try {
                CockpitApi.stream(credentials) { frame ->
                    state = state.copy(stream = frame, streamConnected = true, lastSyncMs = System.currentTimeMillis())
                }
                state = state.copy(streamConnected = false)
            } catch (error: Throwable) {
                if (error is CancellationException) throw error
                if (error is CockpitHttpException && error.status == 401) {
                    onSessionExpired()
                    return@LaunchedEffect
                }
                state = state.copy(streamConnected = false)
            }
            delay(2_000)
        }
    }

    LaunchedEffect(credentials, selectedTab, selectedInstance, foreground) {
        if (!foreground) return@LaunchedEffect
        while (isActive) {
            val result = CockpitApi.loadSection(credentials, selectedTab, selectedInstance.ifBlank { null })
            state = state.copy(pages = state.pages + (selectedTab to result))
            delay(selectedTab.refreshMs)
        }
    }

    LaunchedEffect(credentials, expandedPredictionHistory, predictionHistoryRefresh, foreground, selectedTab) {
        val key = expandedPredictionHistory
        if (!foreground || selectedTab != CockpitTab.PREDICTIONS || key.isBlank()) return@LaunchedEffect
        val parts = key.split(':')
        val asset = parts.getOrNull(0).orEmpty()
        val horizonSeconds = parts.getOrNull(1)?.toIntOrNull() ?: return@LaunchedEffect
        val marketKey = "$asset:$horizonSeconds"
        while (isActive) {
            predictionHistoryLoading = marketKey
            try {
                val payload = CockpitApi.get(credentials, "/api/prediction_history?asset=$asset&horizon_seconds=$horizonSeconds&limit=20")
                predictionHistory = predictionHistory + (marketKey to payload)
                predictionHistoryErrors = predictionHistoryErrors - marketKey
            } catch (error: Throwable) {
                if (error is CancellationException) throw error
                predictionHistoryErrors = predictionHistoryErrors + (marketKey to readable(error))
            } finally {
                if (predictionHistoryLoading == marketKey) predictionHistoryLoading = ""
            }
            delay(30_000)
        }
    }

    val instances = remember(profile, state.pages, state.live) {
        buildList {
            profile?.optJSONArray("instances")?.forEachString { add(it) }
            state.pages[CockpitTab.BOT_FLEET]?.payloads?.get("fleet")?.optJSONArray("fleet")?.forEachObject {
                it.optString("instance").takeIf(String::isNotBlank)?.let(::add)
            }
            state.live?.optJSONObject("status")?.optJSONArray("instances_running")?.forEachString { add(it) }
        }.distinct().sorted()
    }

    val refresh: () -> Unit = {
        scope.launch {
            val query = if (selectedInstance.isBlank()) "" else "?instance=${java.net.URLEncoder.encode(selectedInstance, "UTF-8")}"
            try {
                val live = CockpitApi.get(credentials, "/api/live_lite$query")
                state = state.copy(live = live, liveError = null, lastSyncMs = System.currentTimeMillis())
            } catch (error: Throwable) {
                if (error is CancellationException) throw error
                if (error is CockpitHttpException && error.status == 401) {
                    onSessionExpired()
                    return@launch
                }
                state = state.copy(liveError = readable(error))
            }
            val page = CockpitApi.loadSection(credentials, selectedTab, selectedInstance.ifBlank { null })
            state = state.copy(pages = state.pages + (selectedTab to page))
        }
    }

    Column(
        Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(Color(0xFF0F191D), Night)))
            .windowInsetsPadding(WindowInsets.statusBars).windowInsetsPadding(WindowInsets.navigationBars),
    ) {
        TopBar(
            credentials = credentials,
            profile = profile,
            state = state,
            onRefresh = refresh,
            onSettings = { showSettings = true },
        )
        if (selectedTab in setOf(CockpitTab.OVERVIEW, CockpitTab.MARKETS, CockpitTab.PREDICTIONS)) PriceTicker(state)
        if (selectedTab != CockpitTab.PREDICTIONS) {
            InstancePicker(
                selected = selectedInstance,
                instances = instances,
                mode = state.live?.optJSONObject("status")?.optString("mode").orEmpty(),
                onSelect = { selectedInstance = it },
            )
        }
        CockpitBody(
            tab = selectedTab,
            state = state,
            modifier = Modifier.weight(1f),
            onNavigate = { selectedTab = it },
            predictionAsset = predictionAsset,
            onPredictionAsset = { predictionAsset = it },
            predictionHorizon = predictionHorizon,
            onPredictionHorizon = { predictionHorizon = it },
            predictionWindow = predictionWindow,
            onPredictionWindow = { predictionWindow = it },
            expandedPredictionHistory = expandedPredictionHistory,
            onTogglePredictionHistory = { key -> expandedPredictionHistory = if (expandedPredictionHistory == key) "" else key },
            predictionHistory = predictionHistory,
            predictionHistoryErrors = predictionHistoryErrors,
            predictionHistoryLoading = predictionHistoryLoading,
            onRefreshPredictionHistory = { predictionHistoryRefresh++ },
        )
        MobileNavigationBar(
            selected = selectedTab,
            onSelect = { tab ->
                if (tab == null) showMoreSections = true else selectedTab = tab
            },
        )
    }

    if (showMoreSections) {
        MoreSectionsDialog(
            selected = selectedTab,
            onDismiss = { showMoreSections = false },
            onSelect = { selectedTab = it; showMoreSections = false },
        )
    }

    if (showSettings) {
        SettingsDialog(
            current = credentials,
            saving = settingsSaving,
            error = settingsError,
            onDismiss = { if (!settingsSaving) showSettings = false },
            onSaveServer = { rawUrl, requestPrivateNetwork ->
                settingsSaving = true
                settingsError = null
                try {
                    val updated = credentials.copy(
                        serverUrl = CockpitApi.validateServerUrl(rawUrl),
                        localNetworkPermissionRequested = requestPrivateNetwork,
                    )
                    onConnectToServer(updated, requestPrivateNetwork) { error ->
                        settingsSaving = false
                        if (error == null) showSettings = false else settingsError = error
                    }
                } catch (error: Throwable) {
                    settingsSaving = false
                    settingsError = readable(error)
                }
            },
            onLogout = onLogout,
        )
    }
}

@Composable
private fun TopBar(
    credentials: CockpitCredentials,
    profile: JSONObject?,
    state: ObservatoryState,
    onRefresh: () -> Unit,
    onSettings: () -> Unit,
) {
    val mode = state.live?.optJSONObject("status")?.optString("mode")?.uppercase().orEmpty()
    val serverName = runCatching { URI(credentials.serverUrl).host }.getOrNull().orEmpty()
    Row(
        Modifier.fillMaxWidth().padding(start = 18.dp, end = 14.dp, top = 11.dp, bottom = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            BrandMark()
            Spacer(Modifier.height(5.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                StatusDot(
                    when {
                        state.streamConnected -> Mint
                        state.liveError != null -> Coral
                        state.live != null -> Gold
                        else -> Coral
                    },
                )
                Text(
                    text = when {
                        state.streamConnected -> "LIVE STREAM  ·  ${serverName.ifBlank { "COCKPIT" }}"
                        state.liveError != null && state.live != null -> "DATA STALE  ·  ${serverName.ifBlank { "COCKPIT" }}"
                        state.liveError != null -> "OFFLINE  ·  ${serverName.ifBlank { "COCKPIT" }}"
                        state.live != null -> "CONNECTED  ·  ${serverName.ifBlank { "COCKPIT" }}"
                        else -> "CONNECTING  ·  ${serverName.ifBlank { "COCKPIT" }}"
                    },
                    color = Muted, fontSize = 12.sp, letterSpacing = .75.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (mode.isNotBlank()) {
            Pill(mode, if (mode == "LIVE") Coral else Gold, if (mode == "LIVE") Color(0xFF3B2026) else Color(0xFF332B1C))
            Spacer(Modifier.width(6.dp))
        }
        TextButton(
            onClick = onRefresh,
            modifier = Modifier.semantics { contentDescription = "Refresh cockpit data" },
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 8.dp, vertical = 3.dp),
        ) {
            Text("↻", fontSize = 21.sp, color = Mint)
        }
        TextButton(
            onClick = onSettings,
            modifier = Modifier.semantics { contentDescription = "Cockpit connection settings" },
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 5.dp, vertical = 3.dp),
        ) {
            Text("•••", fontSize = 12.sp, color = Muted, letterSpacing = 1.sp)
        }
    }
}

@Composable
private fun BrandMark() {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(Modifier.size(25.dp).clip(CircleShape).background(MintDim).border(1.dp, Mint.copy(alpha = .45f), CircleShape), contentAlignment = Alignment.Center) {
            Text("✦", color = Mint, fontSize = 12.sp)
        }
        Column(verticalArrangement = Arrangement.spacedBy((-2).dp)) {
            Text("POLYSPARQ++", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.5.sp)
            Text("OBSERVATORY", color = Mint, fontSize = 9.sp, fontWeight = FontWeight.Bold, letterSpacing = 2.4.sp)
        }
    }
}

@Composable
private fun PriceTicker(state: ObservatoryState) {
    val pagePrices = state.pages[CockpitTab.OVERVIEW]?.payloads?.get("prices")?.optJSONObject("prices")
    val streamPrices = state.stream?.optJSONObject("prices")
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())
            .background(Color(0xFF0B1318)).padding(horizontal = 17.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        listOf("BTC", "ETH", "SOL", "XRP").forEach { asset ->
            val quote = streamPrices?.optJSONObject(asset) ?: pagePrices?.optJSONObject(asset)
            val value = quote?.let { firstDouble(it, "oracle", "coinbase", "binance") }
            Column {
                Text(asset, color = Faint, fontSize = 10.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                Text(value?.let(::price) ?: "—", color = if (value == null) Muted else Ink, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            }
        }
        Column {
            Text("STREAM", color = Faint, fontSize = 10.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
            Text(if (state.streamConnected) "0.5s cadence" else "Reconnecting", color = if (state.streamConnected) Mint else Gold, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
private fun MobileNavigationBar(selected: CockpitTab, onSelect: (CockpitTab?) -> Unit) {
    val primary = listOf(
        CockpitTab.OVERVIEW to ("⌂" to "Home"),
        CockpitTab.PREDICTIONS to ("⌁" to "Predict"),
        CockpitTab.MARKETS to ("◉" to "Markets"),
        CockpitTab.BOT_FLEET to ("◈" to "Fleet"),
    )
    Row(
        Modifier.fillMaxWidth().background(Color(0xFF0A1115)).border(1.dp, Stroke.copy(alpha = .65f)),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        primary.forEach { (tab, labels) ->
            val active = selected == tab
            Column(
                Modifier.weight(1f).heightIn(min = 62.dp).clickable { onSelect(tab) }.padding(vertical = 7.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                Text(labels.first, color = if (active) Mint else Muted, fontSize = if (active) 21.sp else 19.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(2.dp))
                Text(labels.second, color = if (active) Mint else Faint, fontSize = 12.sp, fontWeight = if (active) FontWeight.Bold else FontWeight.Medium)
            }
        }
        val moreActive = selected !in primary.map { it.first }
        Column(
            Modifier.weight(1f).heightIn(min = 62.dp).clickable { onSelect(null) }.padding(vertical = 7.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Text("•••", color = if (moreActive) Mint else Muted, fontSize = 18.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
            Spacer(Modifier.height(4.dp))
            Text("More", color = if (moreActive) Mint else Faint, fontSize = 12.sp, fontWeight = if (moreActive) FontWeight.Bold else FontWeight.Medium)
        }
    }
}

@Composable
private fun MoreSectionsDialog(selected: CockpitTab, onDismiss: () -> Unit, onSelect: (CockpitTab) -> Unit) {
    val primary = setOf(CockpitTab.OVERVIEW, CockpitTab.PREDICTIONS, CockpitTab.MARKETS, CockpitTab.BOT_FLEET)
    Dialog(onDismissRequest = onDismiss) {
        Column(
            Modifier.fillMaxWidth().widthIn(max = 440.dp).heightIn(max = 640.dp)
                .clip(RoundedCornerShape(24.dp)).background(SurfaceRaised)
                .border(1.dp, Stroke, RoundedCornerShape(24.dp))
                .verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(9.dp),
        ) {
            Text("All cockpit views", color = Ink, fontSize = 21.sp, fontWeight = FontWeight.Bold)
            Text("Your workspace, organized for a phone.", color = Muted, fontSize = 12.sp)
            Spacer(Modifier.height(3.dp))
            CockpitTab.entries.filter { it !in primary }.forEach { tab ->
                val active = selected == tab
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 54.dp).clip(RoundedCornerShape(15.dp))
                        .background(if (active) MintDim else SurfaceInset)
                        .border(1.dp, if (active) Mint.copy(alpha = .45f) else Stroke, RoundedCornerShape(15.dp))
                        .clickable { onSelect(tab) }.padding(horizontal = 15.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(sectionGlyph(tab), color = if (active) Mint else Aqua, fontSize = 19.sp, modifier = Modifier.width(34.dp))
                    Text(tab.title, Modifier.weight(1f), color = if (active) Ink else Muted, fontSize = 12.sp, fontWeight = if (active) FontWeight.Bold else FontWeight.Medium)
                    Text("›", color = if (active) Mint else Faint, fontSize = 21.sp)
                }
            }
        }
    }
}

private fun sectionGlyph(tab: CockpitTab): String = when (tab) {
    CockpitTab.OVERVIEW -> "⌂"
    CockpitTab.POSITIONS -> "◫"
    CockpitTab.TRADE_HISTORY -> "↗"
    CockpitTab.EXECUTION -> "⇄"
    CockpitTab.MARKETS -> "◉"
    CockpitTab.PREDICTIONS -> "⌁"
    CockpitTab.BOT_FLEET -> "◈"
    CockpitTab.WINDOW_BOTS -> "◷"
    CockpitTab.STRATEGIES -> "⌘"
    CockpitTab.ENGINE_LAB -> "⌬"
    CockpitTab.ACTIVITY_LOGS -> "≋"
}

@Composable
private fun InstancePicker(selected: String, instances: List<String>, mode: String, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().padding(start = 17.dp, end = 17.dp, bottom = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box {
            Row(
                Modifier.clip(RoundedCornerShape(10.dp)).background(SurfaceRaised)
                    .border(1.dp, Stroke, RoundedCornerShape(10.dp)).clickable { expanded = true }
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("SCOPE", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                Spacer(Modifier.width(8.dp))
                Text(selected.ifBlank { "ALL BOTS" }.uppercase(), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.width(6.dp))
                Text("⌄", color = Mint, fontSize = 12.sp)
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }, modifier = Modifier.background(SurfaceRaised)) {
                DropdownMenuItem(text = { Text("All bots", color = Ink) }, onClick = { onSelect(""); expanded = false })
                instances.forEach { instance ->
                    DropdownMenuItem(text = { Text(instance, color = Ink) }, onClick = { onSelect(instance); expanded = false })
                }
            }
        }
        Spacer(Modifier.weight(1f))
        Text("READ ONLY", color = Faint, fontSize = 12.sp, letterSpacing = 1.2.sp)
        if (mode.isNotBlank()) {
            Spacer(Modifier.width(9.dp))
            Text("· ${mode.uppercase()}", color = if (mode.equals("live", true)) Coral else Gold, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun CockpitBody(
    tab: CockpitTab,
    state: ObservatoryState,
    modifier: Modifier,
    onNavigate: (CockpitTab) -> Unit,
    predictionAsset: String,
    onPredictionAsset: (String) -> Unit,
    predictionHorizon: Int,
    onPredictionHorizon: (Int) -> Unit,
    predictionWindow: String,
    onPredictionWindow: (String) -> Unit,
    expandedPredictionHistory: String,
    onTogglePredictionHistory: (String) -> Unit,
    predictionHistory: Map<String, JSONObject>,
    predictionHistoryErrors: Map<String, String>,
    predictionHistoryLoading: String,
    onRefreshPredictionHistory: () -> Unit,
) {
    val section = state.pages[tab] ?: SectionResult()
    Column(modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 18.dp, vertical = 9.dp)) {
        PageHeading(tab, state.lastSyncMs)
        if (state.liveError != null) {
            Notice(
                if (state.live == null) "Waiting for cockpit data" else "Showing the last snapshot",
                state.liveError,
                Coral,
            )
        }
        when (tab) {
            CockpitTab.OVERVIEW -> OverviewPage(state, section, onNavigate)
            CockpitTab.POSITIONS -> PositionsPage(state, section)
            CockpitTab.TRADE_HISTORY -> TradeHistoryPage(section)
            CockpitTab.EXECUTION -> ExecutionPage(section)
            CockpitTab.MARKETS -> MarketsPage(state, section)
            CockpitTab.PREDICTIONS -> PredictionsPage(
                section = section,
                selectedAsset = predictionAsset,
                onAsset = onPredictionAsset,
                selectedHorizon = predictionHorizon,
                onHorizon = onPredictionHorizon,
                selectedWindow = predictionWindow,
                onWindow = onPredictionWindow,
                expandedHistoryKey = expandedPredictionHistory,
                onToggleHistory = onTogglePredictionHistory,
                historyByMarket = predictionHistory,
                historyErrors = predictionHistoryErrors,
                historyLoadingKey = predictionHistoryLoading,
                onRefreshHistory = onRefreshPredictionHistory,
            )
            CockpitTab.BOT_FLEET -> FleetPage(state, section)
            CockpitTab.WINDOW_BOTS -> WindowBotsPage(section)
            CockpitTab.STRATEGIES -> StrategiesPage(section)
            CockpitTab.ENGINE_LAB -> EnginePage(section)
            CockpitTab.ACTIVITY_LOGS -> LogsPage(section)
        }
        section.errors.values.distinct().filterNot { it.contains("admin required", true) || it.contains("not available in demo", true) }
            .forEach { Notice("Partial data", it, Gold) }
        Spacer(Modifier.height(20.dp))
    }
}

@Composable
private fun PageHeading(tab: CockpitTab, updatedAt: Long) {
    Row(Modifier.fillMaxWidth().padding(top = 5.dp, bottom = 14.dp), verticalAlignment = Alignment.Bottom) {
        Column(Modifier.weight(1f)) {
            Text(tab.title.uppercase(), color = Ink, fontSize = 23.sp, fontWeight = FontWeight.SemiBold, letterSpacing = .4.sp)
            Text(tabSubtitle(tab), color = Muted, fontSize = 12.sp, letterSpacing = .55.sp)
        }
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Pill("READ ONLY", Aqua, Color(0xFF142C33))
            Text(if (updatedAt == 0L) "SYNCING" else "UPDATED ${clock(updatedAt)}", color = Faint, fontSize = 11.sp, letterSpacing = .4.sp)
        }
    }
}

@Composable
private fun OverviewPage(state: ObservatoryState, section: SectionResult, onNavigate: (CockpitTab) -> Unit) {
    val live = state.live
    val status = live?.optJSONObject("status")
    val positions = live?.optJSONObject("positions")?.optJSONArray("positions")?.objects().orEmpty()
    val marketData = live?.optJSONObject("markets")?.optJSONArray("markets")?.objects().orEmpty()
    val activity = section.payloads["activity"]
    val fills = state.stream?.optJSONArray("recent_fills")?.objects().orEmpty()
    val recentOrders = activity?.optJSONObject("orders")?.optJSONArray("orders")?.objects().orEmpty()
        .filter { it.optString("status") == "filled" }
    val pnlDays = section.payloads["pnl"]?.optJSONArray("days")?.objects().orEmpty()
    val netPnl = status?.double("net_pnl")
    val mode = status?.optString("mode").orEmpty()

    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Eyebrow("THE POLYSPARQ++ OBSERVATORY")
                Text("Market intelligence.\nAt execution speed.", color = Ink, fontSize = 22.sp, lineHeight = 27.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(5.dp))
                Text("Your capital, strategies and markets — one live view.", color = Muted, fontSize = 12.sp, lineHeight = 16.sp)
            }
            OrbitGraphic(Modifier.size(87.dp))
        }
    }
    if (mode.isNotBlank()) {
        Spacer(Modifier.height(9.dp))
        Panel(background = if (mode.equals("live", true)) Color(0xFF301E25) else Color(0xFF292719)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(9.dp)) {
                StatusDot(if (mode.equals("live", true)) Coral else Gold)
                Text("${mode.uppercase()} MODE", color = if (mode.equals("live", true)) Coral else Gold, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                Text(if (mode.equals("live", true)) "Live execution configured" else "Paper trading safeguards active", color = Muted, fontSize = 12.sp)
            }
        }
    }
    Spacer(Modifier.height(10.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("NET P&L", money(netPnl), pnlTint(netPnl), "${status?.optInt("wins", 0) ?: 0}W  ·  ${status?.optInt("losses", 0) ?: 0}L", Modifier.weight(1f))
        MetricPanel("EQUITY", money(status?.double("equity_usd")), Ink, "Cash ${money(status?.double("cash_usd"))}", Modifier.weight(1f))
    }
    Spacer(Modifier.height(9.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("DEPLOYED", money(status?.double("deployed_usd")), Aqua, "${positions.size} open positions", Modifier.weight(1f))
        MetricPanel("FEES", money(status?.double("fees_usd")), Muted, "Recorded in ledger", Modifier.weight(1f))
    }
    Spacer(Modifier.height(10.dp))
    LineChartPanel("FLEET FILL CASH FLOW", "30 UTC DAYS · AFTER FEES · NOT PROFIT", pnlDays.mapNotNull { it.double("net") })
    Spacer(Modifier.height(10.dp))
    HealthPanel(live?.optJSONObject("health"), status)
    Spacer(Modifier.height(10.dp))
    SectionTitle("Open positions", "${positions.size}", onClick = { onNavigate(CockpitTab.POSITIONS) })
    if (positions.isEmpty()) EmptyPanel("No open positions in this scope", "Position marks arrive from the cockpit feed.")
    positions.take(4).forEach { PositionCard(it) }
    Spacer(Modifier.height(8.dp))
    SectionTitle("Live markets", "${marketData.size}", onClick = { onNavigate(CockpitTab.MARKETS) })
    if (marketData.isEmpty()) EmptyPanel("Market windows will appear here", "Waiting for the server market snapshot.")
    marketData.take(4).forEach { MarketCard(it) }
    Spacer(Modifier.height(8.dp))
    SectionTitle("Latest fills", "${fills.size.coerceAtLeast(recentOrders.size)}", onClick = { onNavigate(CockpitTab.TRADE_HISTORY) })
    val displayFills = if (fills.isNotEmpty()) fills.take(5) else recentOrders.take(5)
    if (displayFills.isEmpty()) EmptyPanel("No recent fills", "The stream will surface new fills as they are recorded.")
    displayFills.forEach { FillCard(it) }
    live?.optJSONArray("radar")?.objects()?.take(5)?.let { alerts ->
        if (alerts.isNotEmpty()) {
            Spacer(Modifier.height(10.dp))
            SectionTitle("Operator radar", alerts.size.toString())
            alerts.forEach { Notice(it.optString("title", "System signal"), it.optString("detail"), when (it.optString("level")) { "critical" -> Coral; "warn" -> Gold; else -> Aqua }) }
        }
    }
}

@Composable
private fun PositionsPage(state: ObservatoryState, section: SectionResult) {
    val status = state.live?.optJSONObject("status")
    val positions = state.live?.optJSONObject("positions")?.optJSONArray("positions")?.objects().orEmpty()
    val orders = section.payloads["activity"]?.optJSONObject("orders")?.optJSONArray("orders")?.objects().orEmpty()
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("OPEN", positions.size.toString(), Mint, "Position ledger", Modifier.weight(1f))
        MetricPanel("DEPLOYED", money(status?.double("deployed_usd")), Aqua, "Current mode", Modifier.weight(1f))
    }
    Spacer(Modifier.height(10.dp))
    LineChartPanel("MARKED P&L", "LIVE POSITION VALUE", positions.mapNotNull { it.double("pnl_usd") })
    Spacer(Modifier.height(12.dp))
    SectionTitle("Current exposure", positions.size.toString())
    if (positions.isEmpty()) EmptyPanel("No open positions", "Positions are reconstructed from the cockpit fill ledger.")
    positions.forEach { PositionCard(it, expanded = true) }
    Spacer(Modifier.height(13.dp))
    SectionTitle("Order activity", "${orders.size}")
    orders.take(20).forEach { OrderCard(it) }
}

@Composable
private fun TradeHistoryPage(section: SectionResult) {
    val history = section.payloads["history"]
    val rows = history?.optJSONArray("rows")?.objects().orEmpty()
    val wins = rows.count { it.optString("result").equals("win", true) }
    val total = rows.size
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("CLOSED", total.toString(), Aqua, "Latest ledger exits", Modifier.weight(1f))
        MetricPanel("WIN RATE", if (total > 0) "${(wins * 100.0 / total).toInt()}%" else "—", Mint, "${wins} wins · ${total - wins} losses/other", Modifier.weight(1f))
    }
    Spacer(Modifier.height(12.dp))
    LineChartPanel("REALIZED P&L", "RECORDED EXIT SEQUENCE", rows.asReversed().mapNotNull { it.double("realized_pnl") })
    Spacer(Modifier.height(12.dp))
    SectionTitle("Closed trades", "${rows.size}")
    if (rows.isEmpty()) EmptyPanel("No closed trades in this view", "The endpoint returns costed exits from the durable fill ledger.")
    rows.forEach { TradeCard(it) }
}

@Composable
private fun ExecutionPage(section: SectionResult) {
    val execution = section.payloads["execution"]
    val mode = execution?.optString("mode").orEmpty().uppercase().ifBlank { "—" }
    val orders = execution?.optJSONArray("open_orders")?.objects().orEmpty()
    val stats = execution?.optJSONArray("stats")?.objects().orEmpty()
    val totalOrders = stats.sumOf { it.double("orders")?.toInt() ?: 0 }
    val filled = stats.sumOf { it.double("filled")?.toInt() ?: 0 }
    val rejected = stats.sumOf { it.double("rejected")?.toInt() ?: 0 }
    val fillRate = if (totalOrders == 0) "—" else "${(100.0 * filled / totalOrders).toInt()}%"
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("MODE", mode, if (mode == "LIVE") Coral else Gold, "Server execution mode", Modifier.weight(1f))
        MetricPanel("FILL RATE", fillRate, Mint, "$filled filled / $totalOrders orders", Modifier.weight(1f))
    }
    Spacer(Modifier.height(9.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("RESTING", orders.size.toString(), Aqua, "Open or partial", Modifier.weight(1f))
        MetricPanel("REJECTED", rejected.toString(), if (rejected == 0) Muted else Coral, "Past 24 hours", Modifier.weight(1f))
    }
    Spacer(Modifier.height(12.dp))
    SectionTitle("Open order queue", "${orders.size}")
    if (orders.isEmpty()) EmptyPanel("No resting orders", "This view refreshes from the execution control plane.")
    orders.forEach { OrderCard(it, execution = true) }
    Spacer(Modifier.height(12.dp))
    SectionTitle("24-hour route summary", "${stats.size}")
    stats.forEach { stat ->
        Panel {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(stat.optString("execution_mode", "unknown").uppercase(), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    Text("${stat.optString("orders", "0")} orders · ${stat.optString("cancelled", "0")} cancelled", color = Muted, fontSize = 12.sp)
                }
                Text(money(stat.double("filled_usd")), color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
        Spacer(Modifier.height(7.dp))
    }
}

@Composable
private fun MarketsPage(state: ObservatoryState, section: SectionResult) {
    val rows = section.payloads["markets"]?.optJSONArray("markets")?.objects()
        ?: state.live?.optJSONObject("markets")?.optJSONArray("markets")?.objects().orEmpty()
    val liveCount = rows.count { it.optString("phase") == "live" }
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("WINDOWS", rows.size.toString(), Aqua, "Markets in snapshot", Modifier.weight(1f))
        MetricPanel("LIVE NOW", liveCount.toString(), Mint, "Book snapshots", Modifier.weight(1f))
    }
    Spacer(Modifier.height(11.dp))
    SectionTitle("Market windows", "LIVE · UPCOMING · CLOSING")
    if (rows.isEmpty()) EmptyPanel("No market windows yet", "Market data is supplied by the cockpit server.")
    rows.forEach { MarketCard(it, expanded = true) }
}

@Composable
private fun PredictionsPage(
    section: SectionResult,
    selectedAsset: String,
    onAsset: (String) -> Unit,
    selectedHorizon: Int,
    onHorizon: (Int) -> Unit,
    selectedWindow: String,
    onWindow: (String) -> Unit,
    expandedHistoryKey: String,
    onToggleHistory: (String) -> Unit,
    historyByMarket: Map<String, JSONObject>,
    historyErrors: Map<String, String>,
    historyLoadingKey: String,
    onRefreshHistory: () -> Unit,
) {
    val board = section.payloads["predictions"]
    val sourceCards = board?.optJSONArray("cards")?.objects().orEmpty()
    val isStale = board?.optBoolean("stale", false) == true
    val model = board?.optString("model_version").orEmpty()
    val assets = sourceCards.map { it.optString("asset") }.filter { it.isNotBlank() }.distinct().ifEmpty { listOf("BTC", "ETH", "SOL", "XRP") }
    val visibleCards = buildList {
        sourceCards.forEach { current ->
            val currentAsset = current.optString("asset")
            val currentHorizon = current.optInt("horizon_seconds")
            val currentMatches = currentHorizon == selectedHorizon && (selectedAsset == "ALL" || selectedAsset == currentAsset)
            if (currentMatches && selectedWindow in setOf("CURRENT", "BOTH")) add("CURRENT" to current)
            val next = current.optJSONObject("next_window")
            val nextMatches = next != null && next.optInt("horizon_seconds", currentHorizon) == selectedHorizon &&
                (selectedAsset == "ALL" || selectedAsset == next.optString("asset", currentAsset))
            if (nextMatches && selectedWindow in setOf("NEXT", "BOTH")) add("NEXT" to next)
        }
    }

    Panel(background = Color(0xFF101C21)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Eyebrow("PREDICTION DESK")
                Text(if (isStale) "Snapshot needs refresh" else if (sourceCards.isEmpty()) "Building live board" else "Live windows, your way", color = Ink, fontSize = 17.sp, fontWeight = FontWeight.Bold)
            }
            StatusDot(if (isStale || sourceCards.isEmpty()) Gold else Mint)
        }
        Spacer(Modifier.height(7.dp))
        Text("${if (model.isBlank()) "Model warming up" else model}  ·  ${if (isStale) "STALE" else "UPDATED ${board?.optDouble("snapshot_age_s", 0.0)?.let { "%.1fs ago".format(Locale.US, it) } ?: "now"}"}", color = Muted, fontSize = 12.sp)
    }
    Spacer(Modifier.height(13.dp))

    HorizonFilterGroup("TIMEFRAME", listOf(300 to "5 min", 900 to "15 min"), selectedHorizon, onHorizon)
    Spacer(Modifier.height(12.dp))
    Text("MARKET", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .9.sp)
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(top = 7.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        (listOf("ALL") + assets).forEach { asset ->
            ChoiceChip(asset, selectedAsset == asset) { onAsset(asset) }
        }
    }
    Spacer(Modifier.height(13.dp))
    WindowFilterGroup("WINDOW", listOf("CURRENT" to "Current", "NEXT" to "Next", "BOTH" to "Both"), selectedWindow, onWindow)
    Spacer(Modifier.height(15.dp))

    board?.optString("disclaimer")?.takeIf { it.isNotBlank() }?.let {
        Notice("Forecast context", "${it.take(420)}${if (it.length > 420) "…" else ""}", Aqua)
    }
    Row(Modifier.fillMaxWidth().padding(top = 2.dp, bottom = 9.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("${if (selectedAsset == "ALL") "All markets" else selectedAsset} · ${if (selectedHorizon == 300) "5 minute" else "15 minute"}", color = Ink, fontSize = 16.sp, fontWeight = FontWeight.Bold)
            Text("${visibleCards.size} ${if (visibleCards.size == 1) "window" else "windows"} · ${selectedWindow.lowercase()} view", color = Muted, fontSize = 12.sp)
        }
        Pill(if (isStale) "STALE" else "LIVE BOARD", if (isStale) Gold else Mint, if (isStale) Color(0xFF332B1C) else MintDim)
    }
    if (sourceCards.isEmpty()) EmptyPanel("Prediction board is warming up", "The cockpit will show forecasts after the next server snapshot.")
    else if (visibleCards.isEmpty()) EmptyPanel("No matching window right now", "Try Both windows or choose another market. The board only shows data returned by the server.")
    visibleCards.forEach { (period, row) ->
        val marketKey = "${row.optString("asset")}:${row.optInt("horizon_seconds", selectedHorizon)}"
        val cardKey = "$marketKey:$period"
        PredictionCard(
            row = row,
            period = period,
            expandedHistory = expandedHistoryKey == cardKey,
            history = historyByMarket[marketKey],
            historyError = historyErrors[marketKey],
            historyLoading = historyLoadingKey == marketKey,
            onToggleHistory = { onToggleHistory(cardKey) },
            onRefreshHistory = onRefreshHistory,
        )
    }
    Text("Forecast probabilities and market prices are context, not a guarantee of profit. Historical checkpoints may be missing or unresolved.", color = Faint, fontSize = 12.sp, lineHeight = 16.sp, modifier = Modifier.padding(top = 5.dp, bottom = 10.dp))
}

@Composable
private fun HorizonFilterGroup(label: String, choices: List<Pair<Int, String>>, selected: Int, onSelect: (Int) -> Unit) {
    Column {
        Text(label, color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .9.sp)
        Row(Modifier.fillMaxWidth().padding(top = 7.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            choices.forEach { (value, title) -> ChoiceChip(title, selected == value, Modifier.weight(1f)) { onSelect(value) } }
        }
    }
}

@Composable
private fun WindowFilterGroup(label: String, choices: List<Pair<String, String>>, selected: String, onSelect: (String) -> Unit) {
    Column {
        Text(label, color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .9.sp)
        Row(Modifier.fillMaxWidth().padding(top = 7.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            choices.forEach { (value, title) -> ChoiceChip(title, selected == value, Modifier.weight(1f)) { onSelect(value) } }
        }
    }
}

@Composable
private fun ChoiceChip(label: String, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(
        modifier.clip(RoundedCornerShape(13.dp))
            .background(if (selected) MintDim else SurfaceInset)
            .border(1.dp, if (selected) Mint.copy(alpha = .55f) else Stroke, RoundedCornerShape(13.dp))
            .clickable(onClick = onClick).heightIn(min = 44.dp).padding(horizontal = 15.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = if (selected) Mint else Muted, fontSize = 12.sp, fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium, maxLines = 1)
    }
}

@Composable
private fun FleetPage(state: ObservatoryState, section: SectionResult) {
    val rows = section.payloads["fleet"]?.optJSONArray("fleet")?.objects().orEmpty()
    val running = rows.count { it.optBoolean("running") }
    val status = state.live?.optJSONObject("status")
    val perfRows = section.payloads["pnl"]?.optJSONArray("rows")?.objects().orEmpty()
    val perfByInstance = perfRows.associateBy { it.optString("instance") }
    val activeRows = rows.filter { it.optBoolean("running") }
    val activePnlRows = activeRows.mapNotNull { perfByInstance[it.optString("instance")] }
    val activePnl = if (activeRows.isNotEmpty() && activePnlRows.size == activeRows.size &&
        activePnlRows.all { it.optBoolean("pnl_available") && it.double("net_pnl") != null }) {
        activePnlRows.sumOf { it.double("net_pnl") ?: 0.0 }
    } else null
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("RUNNING", running.toString(), Mint, "of ${rows.size} configured bots", Modifier.weight(1f))
        MetricPanel("ACTIVE SESSION NET", money(activePnl), pnlTint(activePnl), "All running bots · after fees", Modifier.weight(1f))
    }
    Spacer(Modifier.height(11.dp))
    if (perfRows.isNotEmpty()) {
        SectionTitle("Instance performance", "${perfRows.size}")
        perfRows.forEach { perf ->
            Panel {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(perf.optString("instance", "Bot"), Modifier.weight(1f), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    val value = perf.double("net_pnl").takeIf { perf.optBoolean("pnl_available") }
                    Text(money(value), color = pnlTint(value), fontSize = 12.sp, fontWeight = FontWeight.Bold)
                }
                Text("${perf.optString("accounting_status", "unverified").replace('_', ' ').uppercase()} · ${perf.optInt("fills")} fills · ${perf.optInt("wins")}W/${perf.optInt("losses")}L · after fees", color = Muted, fontSize = 12.sp)
            }
            Spacer(Modifier.height(7.dp))
        }
    }
    SectionTitle("Bot fleet", "${rows.size}")
    if (rows.isEmpty()) EmptyPanel("No bot instances returned", "Fleet visibility follows the permissions of this cockpit account.")
    rows.forEach { FleetCard(it, perf = perfByInstance[it.optString("instance")]) }
}

@Composable
private fun WindowBotsPage(section: SectionResult) {
    val payload = section.payloads["window_bots"]
    val rows = payload?.optJSONArray("bots")?.objects().orEmpty()
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("WINDOW BOTS", rows.size.toString(), Aqua, "Configured profiles", Modifier.weight(1f))
        MetricPanel("ASSETS", rows.flatMap { it.optJSONArray("assets")?.strings().orEmpty() }.distinct().size.toString(), Mint, "Across active profiles", Modifier.weight(1f))
    }
    Spacer(Modifier.height(11.dp))
    SectionTitle("Window strategy desk", rows.size.toString())
    if (rows.isEmpty()) EmptyPanel("No window bots in this account", "The root cockpit returns the profiles visible to this user.")
    rows.forEach { WindowBotCard(it) }
}

@Composable
private fun StrategiesPage(section: SectionResult) {
    val fleet = section.payloads["fleet"]?.optJSONArray("fleet")?.objects().orEmpty()
    val feed = section.payloads["strategies"]
    val history = feed?.optJSONArray("history")?.objects().orEmpty()
    val global = feed?.optJSONArray("feed")?.objects().orEmpty()
    SectionTitle("Strategy instances", fleet.size.toString())
    if (fleet.isEmpty()) EmptyPanel("No strategy instances returned", "This view uses the root cockpit fleet and strategy history APIs.")
    fleet.forEach { FleetCard(it, showStrategies = true) }
    Spacer(Modifier.height(10.dp))
    SectionTitle("Recent strategy decisions", (history.size + global.size).toString())
    if (history.isEmpty() && global.isEmpty()) {
        if (section.errors["strategies"]?.contains("admin required", true) == true) {
            Notice("Account-limited feed", "The all-bot supervisor history is restricted to cockpit administrators. Per-instance strategy summaries are shown above.", Gold)
        } else EmptyPanel("No recent strategy decisions", "New decisions appear here when recorded by the cockpit.")
    }
    (history + global).take(30).forEach { StrategyDecisionCard(it) }
}

@Composable
private fun EnginePage(section: SectionResult) {
    val predictor = section.payloads["predictor"]
    val engine = predictor?.optJSONObject("engine")
    val summary = predictor?.optJSONObject("summary")
    val predictions = predictor?.optJSONArray("predictions")?.objects().orEmpty()
    val signals = section.payloads["signals"]?.optJSONArray("signals")?.objects().orEmpty()
    val quality = section.payloads["performance"]
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("PREDICTOR", if (engine?.optBoolean("running") == true) "RUNNING" else "IDLE", if (engine?.optBoolean("running") == true) Mint else Gold, "Configured engine state", Modifier.weight(1f))
        MetricPanel("SETTLED", summary?.optString("settled", "0") ?: "—", Aqua, "Resolved checkpoints", Modifier.weight(1f))
    }
    Spacer(Modifier.height(9.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        MetricPanel("ACCURACY", summary?.double("accuracy")?.let { "${(it * 100).toInt()}%" } ?: "—", Ink, "Settled predictions", Modifier.weight(1f))
        MetricPanel("RECORDED P&L", money(summary?.double("pnl")), pnlTint(summary?.double("pnl")), "Prediction ledger", Modifier.weight(1f))
    }
    Spacer(Modifier.height(11.dp))
    quality?.let { perf ->
        Panel {
            Eyebrow("ENGINE QUALITY")
            Text("${perf.optString("total_entries", "0")} entries · ${perf.optString("total_exits", "0")} exits · fees ${money(perf.double("total_fees"))}", color = Ink, fontSize = 12.sp)
            Text("Evaluation fields reflect the server report; an accuracy figure alone does not establish a trading edge.", color = Muted, fontSize = 12.sp, lineHeight = 14.sp)
        }
        Spacer(Modifier.height(10.dp))
    }
    SectionTitle("Predictor checkpoints", "${predictions.size}")
    if (predictions.isEmpty()) EmptyPanel("No predictor checkpoints returned", "The predictor feed can be empty when the strategy is disabled or warming up.")
    predictions.take(40).forEach { PredictionHistoryCard(it) }
    Spacer(Modifier.height(10.dp))
    SectionTitle("Signal history", signals.size.toString())
    signals.take(30).forEach { SignalCard(it) }
}

@Composable
private fun LogsPage(section: SectionResult) {
    val logs = section.payloads["logs"]?.optJSONArray("lines")?.strings().orEmpty()
    val feed = section.payloads["supervisor_feed"]?.optJSONArray("feed")?.objects().orEmpty()
    val exchanges = section.payloads["supervisor_log"]?.optJSONArray("exchanges")?.objects().orEmpty()
    SectionTitle("Bot log tail", "${logs.size} lines")
    if (logs.isEmpty()) {
        val restricted = section.errors["supervisor_feed"]?.contains("admin required", true) == true
        EmptyPanel(if (restricted) "Choose an instance for bot logs" else "No log lines returned", if (restricted) "The all-bot supervisor feed is admin-only. Instance-scoped logs are available when the server account allows them." else "Log lines are supplied redacted by the cockpit API.")
    }
    if (logs.isNotEmpty()) {
        Panel(background = Color(0xFF0A1014)) {
            logs.take(100).forEachIndexed { index, line ->
                Text(line.take(280), color = if (line.contains("error", true) || line.contains("fatal", true)) Coral else if (line.contains("warn", true)) Gold else Muted, fontSize = 12.sp, lineHeight = 14.sp, fontFamily = FontFamily.Monospace)
                if (index < logs.size - 1) Spacer(Modifier.height(6.dp))
            }
        }
    }
    if (feed.isNotEmpty()) {
        Spacer(Modifier.height(12.dp))
        SectionTitle("Strategy supervisor", feed.size.toString())
        feed.forEach { StrategyDecisionCard(it) }
    }
    if (exchanges.isNotEmpty()) {
        Spacer(Modifier.height(12.dp))
        SectionTitle("Supervisor exchanges", exchanges.size.toString())
        exchanges.forEach { SignalCard(it) }
    }
}

@Composable
private fun PositionCard(row: JSONObject, expanded: Boolean = false) {
    val pnl = row.double("pnl_usd")
    val side = row.optString("side", "—").uppercase()
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Pill(side, if (side == "UP") Mint else Coral, if (side == "UP") MintDim else Color(0xFF3A2026))
            Column(Modifier.weight(1f)) {
                Text("${row.optString("asset", "Market")}  ·  ${row.optString("instance", "default")}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${num(row.double("shares"), 2)} shares  ·  avg ${price(row.double("avg"))}  ·  mark ${price(row.double("mark"))}", color = Muted, fontSize = 12.sp)
            }
            Text(money(pnl), color = pnlTint(pnl), fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
        if (expanded) {
            Spacer(Modifier.height(9.dp))
            Hairline()
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                TinyValue("COST BASIS", money(row.double("cost_basis")))
                TinyValue("MARK SOURCE", row.optString("mark_source", "—"))
                TinyValue("WINDOW", duration(row.double("seconds_left")))
            }
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun MarketCard(row: JSONObject, expanded: Boolean = false) {
    val up = row.double("up_mid") ?: row.double("polymarket_up")
    val down = row.double("down_mid") ?: row.double("polymarket_down")
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                    Text(row.optString("asset", "MARKET"), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    Pill(horizon(row.double("horizon_seconds")), Aqua, Color(0xFF142C33))
                    Pill(row.optString("phase", "active").uppercase(), if (row.optString("phase") == "live") Mint else Gold, if (row.optString("phase") == "live") MintDim else Color(0xFF332B1C))
                }
                Spacer(Modifier.height(4.dp))
                Text(row.optString("question", "Market window"), color = Muted, fontSize = 12.sp, lineHeight = 13.sp, maxLines = if (expanded) 3 else 2, overflow = TextOverflow.Ellipsis)
            }
            Spacer(Modifier.width(9.dp))
            Column(horizontalAlignment = Alignment.End) {
                Text("UP", color = Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(probability(up), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (expanded) {
            Spacer(Modifier.height(10.dp))
            ProbabilityTrack(up)
            Spacer(Modifier.height(8.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                TinyValue("UP BOOK", "${price(row.double("up_bid"))} / ${price(row.double("up_ask"))}")
                TinyValue("DOWN", probability(down))
                TinyValue("CLOSE", clockValue(row["window_close"]))
            }
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun FillCard(row: JSONObject) {
    val direction = row.optString("direction", "fill").uppercase()
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(9.dp)) {
            Text(if (direction == "BUY") "↓" else "↑", color = if (direction == "BUY") Mint else Gold, fontSize = 17.sp, fontWeight = FontWeight.Bold)
            Column(Modifier.weight(1f)) {
                Text("$direction  ${row.optString("asset", "?")} ${row.optString("side_label", "")}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text("${row.optString("instance", row.optString("source", "cockpit"))} · ${clockValue(row["ts"] ?: row["ts_created"])}", color = Muted, fontSize = 12.sp)
            }
            Text(price(row.double("price")), color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
    }
    Spacer(Modifier.height(6.dp))
}

@Composable
private fun OrderCard(row: JSONObject, execution: Boolean = false) {
    val status = row.optString("status", "open").uppercase()
    val side = row.optString("side_label", "")
    val direction = row.optString("direction", "")
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("${row.optString("asset", "ORDER")} ${horizon(row.double("horizon_seconds"))} · $direction $side", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${row.optString("instance", row.optString("source", "—"))} · ${row.optString("execution_mode", if (row.optBoolean("post_only")) "maker" else "taker").uppercase()} · ${clockValue(row["ts_submitted"] ?: row["ts_created"])}", color = Muted, fontSize = 12.sp)
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(price(row.double("price")), color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(status, color = when (status) { "REJECTED" -> Coral; "FILLED" -> Mint; else -> Gold }, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (execution) {
            Spacer(Modifier.height(8.dp))
            Hairline()
            Spacer(Modifier.height(7.dp))
            val remaining = (row.double("size_shares") ?: 0.0) - (row.double("matched_shares") ?: 0.0)
            Text("${num(remaining.coerceAtLeast(0.0), 2)} shares remaining of ${num(row.double("size_shares"), 2)} · ${money(row.double("notional_usd"))} notional", color = Muted, fontSize = 12.sp)
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun TradeCard(row: JSONObject) {
    val pnl = row.double("realized_pnl")
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("${row.optString("asset", "TRADE")} ${row.optString("side_label", "")} · ${horizon(row.double("horizon_seconds"))}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(row.optString("question", "${row.optString("instance", "bot")} · ${row.optString("source", "exit")}"), color = Muted, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text("${row.optString("result", "unverified").uppercase()} · ${clockValue(row["exit_ts"] ?: row["ts_created"])}", color = Faint, fontSize = 12.sp, letterSpacing = .4.sp)
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(money(pnl), color = pnlTint(pnl), fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text("fees ${money(row.double("fee_usd"))}", color = Muted, fontSize = 12.sp)
            }
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun PredictionCard(
    row: JSONObject,
    period: String,
    expandedHistory: Boolean,
    history: JSONObject?,
    historyError: String?,
    historyLoading: Boolean,
    onToggleHistory: () -> Unit,
    onRefreshHistory: () -> Unit,
) {
    val stale = row.optString("model_status").contains("stale", true) || row.optString("status").contains("stale", true)
    val direction = row.optString("independent_direction", row.optString("direction", "WAIT"))
    val modelP = row.double("independent_up_probability") ?: row.double("settlement_model_probability")
    val marketP = row.double("polymarket_up") ?: row.double("polymarket_up_price")
    val gap = if (modelP != null && marketP != null) (modelP - marketP) * 100.0 else null
    val countdown = if (period == "NEXT") duration(row.double("seconds_until_open")) + " to open" else duration(row.double("seconds_left")) + " left"
    val source = row.optString("reference", row.optString("settlement_source", "Reference source unavailable"))
    Panel(background = Color(0xFF111D22)) {
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                    Text(row.optString("asset", "MARKET"), color = Ink, fontSize = 17.sp, fontWeight = FontWeight.Bold)
                    Pill(horizon(row.double("horizon_seconds")), Aqua, Color(0xFF142C33))
                }
                Spacer(Modifier.height(5.dp))
                Text(row.optString("question", "Market window"), color = Muted, fontSize = 12.sp, lineHeight = 17.sp, maxLines = 3, overflow = TextOverflow.Ellipsis)
            }
            Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Pill(if (period == "NEXT") "NEXT WINDOW" else "CURRENT", if (period == "NEXT") Aqua else Mint, if (period == "NEXT") Color(0xFF142C33) else MintDim)
                Text(countdown, color = if (period == "NEXT") Aqua else Gold, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
        }
        Spacer(Modifier.height(13.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("${direction.uppercase()}", color = if (direction.equals("UP", true)) Mint else if (direction.equals("DOWN", true)) Coral else Gold, fontSize = 20.sp, fontWeight = FontWeight.ExtraBold)
            Spacer(Modifier.weight(1f))
            Pill(if (stale) "STALE DATA" else row.optString("model_status", "WAIT").replace('_', ' ').uppercase().take(20), if (stale) Gold else Mint, if (stale) Color(0xFF332B1C) else MintDim)
        }
        Spacer(Modifier.height(11.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(9.dp)) {
            ProbabilityTile("MODEL P(UP)", probability(modelP), Mint, Modifier.weight(1f))
            ProbabilityTile("MARKET MID", probability(marketP), Aqua, Modifier.weight(1f))
        }
        if (modelP != null) {
            Spacer(Modifier.height(9.dp))
            ProbabilityTrack(modelP)
            Row(Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Model P(UP)", color = Muted, fontSize = 12.sp)
                Spacer(Modifier.weight(1f))
                Text("${if (modelP >= .5) "UP" else "DOWN"} ${probability(modelP)}", color = Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
        Spacer(Modifier.height(9.dp))
        Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(SurfaceInset).padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("MODEL − MARKET", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(gap?.let { String.format(Locale.US, "%+.1f percentage points", it) } ?: "Unavailable until both values are fresh", color = if (gap == null) Muted else if (gap > 0) Mint else Coral, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
            Text("context only", color = Faint, fontSize = 12.sp)
        }
        Spacer(Modifier.height(10.dp))
        Text(row.optString("prediction_basis", row.optString("manual_note", "Forecast and market reference are shown separately.")), color = Muted, fontSize = 12.sp, lineHeight = 16.sp, maxLines = 4, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(7.dp))
        Text("REFERENCE  ·  $source", color = Faint, fontSize = 12.sp, lineHeight = 14.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        val freshness = listOf(
            "Ref ${row.double("reference_age_s")?.let { "%.1fs".format(Locale.US, it) } ?: "—"}",
            "Book ${row.double("book_age_s")?.let { "%.1fs".format(Locale.US, it) } ?: "—"}",
            "Candle ${row.double("candle_age_s")?.let { "%.0fs".format(Locale.US, it) } ?: "—"}",
        ).joinToString("  ·  ")
        Spacer(Modifier.height(7.dp))
        Text(freshness, color = Faint, fontSize = 12.sp)
        Spacer(Modifier.height(11.dp))
        Row(
            Modifier.fillMaxWidth().heightIn(min = 46.dp).clip(RoundedCornerShape(13.dp))
                .background(if (expandedHistory) MintDim else SurfaceRaised)
                .border(1.dp, if (expandedHistory) Mint.copy(alpha = .45f) else Stroke, RoundedCornerShape(13.dp))
                .clickable(onClick = onToggleHistory).padding(horizontal = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(if (expandedHistory) "▾" else "▸", color = Mint, fontSize = 18.sp)
            Spacer(Modifier.width(10.dp))
            Text(if (expandedHistory) "Hide window history" else "View ${row.optString("asset")} ${horizon(row.double("horizon_seconds"))} history", Modifier.weight(1f), color = if (expandedHistory) Mint else Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            Text("↻", color = Aqua, fontSize = 17.sp)
        }
        if (expandedHistory) {
            Spacer(Modifier.height(11.dp))
            when {
                historyLoading && history == null -> Notice("Loading recorded checkpoints", "Fetching saved predictions and outcomes for ${row.optString("asset")} ${horizon(row.double("horizon_seconds"))}.", Aqua)
                historyError != null && history == null -> Notice("History unavailable", historyError, Gold)
                history != null -> PredictionHistoryPanel(history, historyError, onRefreshHistory)
                else -> Text("History is being prepared by the cockpit.", color = Muted, fontSize = 12.sp)
            }
        }
    }
    Spacer(Modifier.height(12.dp))
}

@Composable
private fun PredictionHistoryPanel(payload: JSONObject, error: String?, onRefresh: () -> Unit) {
    val rows = payload.optJSONArray("rows")?.objects().orEmpty()
    val total = payload.optInt("total", rows.size)
    val resolved = rows.count { !it.isNull("outcome") && it.optString("outcome").isNotBlank() }
    val latest = rows.firstOrNull()
    val checkpoints = latest?.optJSONArray("checkpoints")?.objects().orEmpty()
        .filter { it.optString("status") == "captured" && it.double("up_probability") != null }
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(15.dp)).background(Color(0xFF0B1418)).padding(13.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("WINDOW HISTORY", color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .8.sp)
                Text("$total recorded · $resolved resolved in this page", color = Muted, fontSize = 12.sp)
            }
            TextButton(onClick = onRefresh) { Text("REFRESH", color = Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold) }
        }
        if (error != null) {
            Spacer(Modifier.height(6.dp))
            Text("Latest refresh failed: $error", color = Gold, fontSize = 12.sp, lineHeight = 14.sp)
        }
        if (payload.optBoolean("importing")) {
            Spacer(Modifier.height(5.dp))
            Text("Legacy journal import is still running.", color = Gold, fontSize = 12.sp)
        }
        if (rows.isEmpty()) {
            Spacer(Modifier.height(10.dp))
            Text("No persisted windows yet. New windows and forecast checkpoints are recorded by the cockpit as they arrive.", color = Muted, fontSize = 12.sp, lineHeight = 17.sp)
        } else {
            Spacer(Modifier.height(12.dp))
            Text("RECENT SETTLEMENTS", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .7.sp)
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(top = 8.dp, bottom = 5.dp), horizontalArrangement = Arrangement.spacedBy(7.dp)) {
                rows.take(12).asReversed().forEach { window ->
                    val outcome = window.optString("outcome")
                    val tint = when (outcome) { "UP" -> Mint; "DOWN" -> Coral; else -> Gold }
                    Column(
                        Modifier.width(55.dp).clip(RoundedCornerShape(11.dp)).background(SurfaceInset).padding(vertical = 8.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(when (outcome) { "UP" -> "↑"; "DOWN" -> "↓"; else -> "·" }, color = tint, fontSize = 19.sp, fontWeight = FontWeight.Bold)
                        Text(clockValue(window.opt("window_close_ms")), color = Muted, fontSize = 12.sp, maxLines = 1)
                    }
                }
            }
            if (checkpoints.isNotEmpty()) {
                Spacer(Modifier.height(7.dp))
                Text("LATEST WINDOW · MODEL P(UP) CHECKPOINTS", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .45.sp)
                Spacer(Modifier.height(6.dp))
                CheckpointChart(checkpoints, latest?.optString("outcome").orEmpty())
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    checkpoints.forEach { checkpoint ->
                        val label = checkpoint.optString("key").replace("elapsed_", "").replace("pct", "%").replace("open_0%", "OPEN")
                        Column(Modifier.widthIn(min = 55.dp)) {
                            Text(label, color = Faint, fontSize = 12.sp, maxLines = 1)
                            Text(probability(checkpoint.double("up_probability")), color = Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                        }
                    }
                }
            } else {
                Spacer(Modifier.height(9.dp))
                Text("No captured probability checkpoints for the newest window yet.", color = Muted, fontSize = 12.sp)
            }
            Spacer(Modifier.height(12.dp))
            Text("LATEST WINDOWS", color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .7.sp)
            rows.take(6).forEach { window ->
                val outcome = window.optString("outcome").ifBlank { "PENDING" }
                val tint = when (outcome) { "UP" -> Mint; "DOWN" -> Coral; else -> Gold }
                val count = window.optJSONArray("checkpoints")?.objects()?.count { it.optString("status") == "captured" } ?: 0
                Row(Modifier.fillMaxWidth().padding(top = 9.dp), verticalAlignment = Alignment.CenterVertically) {
                    StatusDot(tint)
                    Spacer(Modifier.width(8.dp))
                    Column(Modifier.weight(1f)) {
                        Text("${clockValue(window.opt("window_open_ms"))}  ·  ${window.optString("model_version", "model unknown")}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                        Text("Open ${price(window.double("open_price"))}  ·  Close ${price(window.double("close_price"))}  ·  $count checkpoints", color = Muted, fontSize = 12.sp)
                    }
                    Text(outcome, color = tint, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

@Composable
private fun CheckpointChart(checkpoints: List<JSONObject>, outcome: String) {
    val values = checkpoints.mapNotNull { it.double("up_probability")?.toFloat() }
    val lineColor = if (outcome == "DOWN") Coral else Mint
    Canvas(
        Modifier.fillMaxWidth().height(122.dp).semantics {
            contentDescription = "Latest forecast checkpoint chart: ${values.joinToString { probability(it.toDouble()) }}; outcome ${outcome.ifBlank { "pending" }}"
        },
    ) {
        val left = 7.dp.toPx()
        val right = size.width - 7.dp.toPx()
        val top = 8.dp.toPx()
        val bottom = size.height - 8.dp.toPx()
        listOf(.25f, .5f, .75f).forEach { level ->
            val y = bottom - level * (bottom - top)
            drawLine(if (level == .5f) Gold.copy(alpha = .42f) else Stroke.copy(alpha = .65f), Offset(left, y), Offset(right, y), strokeWidth = 1.dp.toPx())
        }
        if (values.isNotEmpty()) {
            val path = Path()
            val dx = if (values.size < 2) 0f else (right - left) / (values.size - 1)
            val points = values.mapIndexed { index, value ->
                Offset(left + index * dx, bottom - value.coerceIn(0f, 1f) * (bottom - top))
            }
            points.forEachIndexed { index, point -> if (index == 0) path.moveTo(point.x, point.y) else path.lineTo(point.x, point.y) }
            if (points.size > 1) drawPath(path, lineColor, style = DrawStroke(width = 2.5.dp.toPx()))
            points.forEach { point -> drawCircle(lineColor, radius = 4.dp.toPx(), center = point) }
        }
    }
}

@Composable
private fun FleetCard(row: JSONObject, showStrategies: Boolean = false, perf: JSONObject? = null) {
    val running = row.optBoolean("running")
    val strategy = row.optJSONObject("strategy")
    val recent = row.optJSONArray("recent_strategies")?.objects().orEmpty()
    val wallet = row.optJSONObject("wallet")
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusDot(if (running) Mint else Faint)
            Spacer(Modifier.width(9.dp))
            Column(Modifier.weight(1f)) {
                Text(row.optString("name", row.optString("instance", "Bot")), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${row.optString("instance", "—")} · ${row.optString("current_config", "")}", color = Muted, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Pill(if (running) "RUNNING" else "STOPPED", if (running) Mint else Muted, if (running) MintDim else SurfaceInset)
        }
        Spacer(Modifier.height(9.dp))
        Hairline()
        Spacer(Modifier.height(8.dp))
        if (!showStrategies) {
            val pnl = perf?.double("net_pnl")?.takeIf { perf.optBoolean("pnl_available") }
            val liveScope = perf?.optString("pnl_scope")?.startsWith("live_") == true
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Column {
                    Text(if (liveScope) "LIFETIME NET · AFTER FEES" else "SESSION NET · AFTER FEES", color = Faint, fontSize = 12.sp, letterSpacing = .6.sp)
                    Text(money(pnl), color = pnlTint(pnl), fontSize = 12.sp, fontWeight = FontWeight.Bold)
                }
                Text(perf?.optString("accounting_status", "no session data")?.replace('_', ' ')?.uppercase() ?: "P&L UNAVAILABLE", color = if (pnl == null) Muted else Mint, fontSize = 12.sp, letterSpacing = .4.sp)
            }
            Spacer(Modifier.height(8.dp))
        }
        val title = strategy?.optString("name").orEmpty().ifBlank { strategy?.optString("strategy_name").orEmpty() }
        if (title.isNotBlank()) Text(title, color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            TinyValue("MODE", strategy?.optString("mode", "—") ?: "—")
            TinyValue("SUPERVISOR", if (row.optJSONObject("supervisor")?.optBoolean("running") == true) "ON" else "OFF")
            TinyValue("PAPER WALLET", wallet?.optString("mode", "—")?.uppercase() ?: "—")
        }
        if (showStrategies && recent.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text("RECENT CHANGES", color = Faint, fontSize = 12.sp, letterSpacing = .8.sp)
            recent.take(3).forEach { Text("• ${it.optString("reason", "Strategy update")}", color = Muted, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis) }
        }
    }
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun WindowBotCard(row: JSONObject) {
    val exit = row.optJSONObject("exit_24h")
    val gates = row.optJSONObject("entry_gate")
    val assets = row.optJSONArray("assets")?.strings().orEmpty()
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(row.optString("strategy_name", row.optString("instance", "Window bot")), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(row.optString("instance", "—"), color = Muted, fontSize = 12.sp)
            }
            Pill(if (row.optBoolean("running")) "RUNNING" else "STOPPED", if (row.optBoolean("running")) Mint else Muted, if (row.optBoolean("running")) MintDim else SurfaceInset)
        }
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            assets.take(4).forEach { Pill(it, Aqua, Color(0xFF142C33)) }
            Pill(row.optString("mode", "unknown").uppercase(), if (row.optString("mode") == "live") Coral else Gold, SurfaceInset)
        }
        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            TinyValue("24H EXIT P&L", money(exit?.double("net")))
            TinyValue("EXITS", exit?.optString("count", "0") ?: "—")
            TinyValue("GATE CHECKS", gates?.optString("checks_60s", "0") ?: "—")
        }
        gates?.optJSONArray("latest_by_asset")?.objects()?.take(4)?.forEach { gate ->
            Text("${gate.optString("asset")} · ${gate.optString("state")} · ${gate.optString("reason")} · ${duration(gate.double("seconds_left"))} left", color = Muted, fontSize = 12.sp)
        }
    }
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun StrategyDecisionCard(row: JSONObject) {
    val reason = row.optString("reason", row.optString("last_decision", "Strategy decision recorded"))
    Panel {
        Row(verticalAlignment = Alignment.Top) {
            Text("✳", color = Aqua, fontSize = 12.sp)
            Spacer(Modifier.width(9.dp))
            Column(Modifier.weight(1f)) {
                Text("${row.optString("instance", row.optString("name", "SUPERVISOR"))} · ${row.optString("model", "strategy")}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(reason, color = Muted, fontSize = 12.sp, lineHeight = 14.sp)
                Text(clockValue(row["ts"] ?: row["created_at"]), color = Faint, fontSize = 12.sp)
            }
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun PredictionHistoryCard(row: JSONObject) {
    val correct = if (row.has("correct") && !row.isNull("correct")) row.optBoolean("correct") else null
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("${row.optString("asset", "CHECKPOINT")} ${row.optString("side_label", "")}", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(row.optString("question", "Predictor checkpoint"), color = Muted, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(probability(row.double("confidence") ?: row.double("sig_confidence")), color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                Text(if (correct == null) "PENDING" else if (correct) "CORRECT" else "INCORRECT", color = if (correct == null) Gold else if (correct) Mint else Coral, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
    }
    Spacer(Modifier.height(6.dp))
}

@Composable
private fun SignalCard(row: JSONObject) {
    Panel {
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Text(row.optString("model_version", row.optString("trigger", "SIGNAL")), color = Aqua, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .5.sp)
                Text(row.optString("reason", row.optString("error", row.optString("response", row.optString("name", "Signal recorded")))), color = Ink, fontSize = 12.sp, lineHeight = 13.sp, maxLines = 4, overflow = TextOverflow.Ellipsis)
                Text("${row.optString("instance", "engine")} · ${clockValue(row["ts"] ?: row["created_at"])}", color = Faint, fontSize = 12.sp)
            }
            if (row.has("latency_ms")) Text("${row.optString("latency_ms")}ms", color = Muted, fontSize = 12.sp)
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun HealthPanel(health: JSONObject?, status: JSONObject?) {
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Eyebrow("SYSTEM HEALTH")
                Text(if (status?.optBoolean("running") == true) "Fleet heartbeat received" else "No running bot reported", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
            StatusDot(if (status?.optBoolean("running") == true) Mint else Gold)
        }
        Spacer(Modifier.height(9.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            TinyValue("BOOKS", health?.optString("orderbook_health", "—") ?: "—")
            TinyValue("ORACLE", health?.optString("oracle_source", "—") ?: "—")
            TinyValue("HEARTBEAT", clockValue(health?.opt("heartbeat")))
        }
        val feeds = health?.optJSONObject("ws")
        if (feeds != null && feeds.length() > 0) {
            Spacer(Modifier.height(7.dp))
            Text(feeds.keys().asSequence().take(4).joinToString("  ·  ") { "$it ${feeds.optString(it)}" }, color = Faint, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun ProbabilityTrack(value: Double?) {
    val fraction = value?.coerceIn(0.0, 1.0)?.toFloat() ?: 0f
    Canvas(Modifier.fillMaxWidth().height(8.dp).clip(CircleShape)) {
        drawRoundRect(Color(0xFF293A40), cornerRadius = androidx.compose.ui.geometry.CornerRadius(size.height / 2))
        if (value != null) drawRoundRect(Brush.horizontalGradient(listOf(Mint, Aqua)), size = size.copy(width = size.width * fraction), cornerRadius = androidx.compose.ui.geometry.CornerRadius(size.height / 2))
    }
}

@Composable
private fun ProbabilityTile(label: String, value: String, color: Color, modifier: Modifier) {
    Column(modifier.clip(RoundedCornerShape(11.dp)).background(SurfaceInset).padding(12.dp)) {
        Text(label, color = Faint, fontSize = 12.sp, letterSpacing = .55.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(value, color = color, fontSize = 16.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun LineChartPanel(title: String, subtitle: String, values: List<Double>) {
    Panel {
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Eyebrow(title)
                Text(subtitle, color = Faint, fontSize = 12.sp, letterSpacing = .55.sp)
            }
            if (values.isNotEmpty()) Text(money(values.last()), color = pnlTint(values.last()), fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(10.dp))
        Canvas(
            Modifier.fillMaxWidth().height(132.dp).semantics {
                val summary = values.takeLast(8).joinToString(", ") { money(it) }
                contentDescription = if (summary.isBlank()) "$title chart, no recorded values" else "$title chart, latest values $summary"
            },
        ) {
            val left = 2.dp.toPx()
            val right = size.width - 2.dp.toPx()
            val top = 5.dp.toPx()
            val bottom = size.height - 8.dp.toPx()
            repeat(4) { index ->
                val y = top + (bottom - top) * index / 3f
                drawLine(Stroke.copy(alpha = .75f), Offset(left, y), Offset(right, y), strokeWidth = 1.dp.toPx())
            }
            if (values.size > 1) {
                val min = minOf(0.0, values.minOrNull() ?: 0.0)
                val max = maxOf(0.0, values.maxOrNull() ?: 0.0)
                val spread = (max - min).takeIf { it > 0.000001 } ?: 1.0
                val points = values.mapIndexed { index, value ->
                    val x = left + (right - left) * index / (values.size - 1).toFloat()
                    val y = bottom - (((value - min) / spread).toFloat() * (bottom - top))
                    Offset(x, y)
                }
                val area = Path().apply {
                    moveTo(points.first().x, bottom)
                    points.forEach { lineTo(it.x, it.y) }
                    lineTo(points.last().x, bottom)
                    close()
                }
                drawPath(area, Brush.verticalGradient(listOf(Mint.copy(alpha = .25f), Mint.copy(alpha = .005f)), top, bottom))
                val line = Path().apply { moveTo(points.first().x, points.first().y); points.drop(1).forEach { lineTo(it.x, it.y) } }
                drawPath(line, Mint, style = DrawStroke(width = 2.5.dp.toPx(), cap = androidx.compose.ui.graphics.StrokeCap.Round))
                val last = points.last()
                drawCircle(Mint, radius = 4.dp.toPx(), center = last)
            } else {
                drawLine(Mint.copy(alpha = .5f), Offset(left, (top + bottom) / 2), Offset(right, (top + bottom) / 2), strokeWidth = 2.dp.toPx())
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(if (values.isEmpty()) "Waiting for recorded data" else "${values.size} server points", color = Faint, fontSize = 12.sp)
            Text("LIVE FEED", color = Mint.copy(alpha = .75f), fontSize = 12.sp, letterSpacing = .8.sp)
        }
    }
}

@Composable
private fun MetricPanel(label: String, value: String, tint: Color, detail: String, modifier: Modifier) {
    Column(
        modifier.clip(RoundedCornerShape(16.dp)).background(Surface)
            .border(1.dp, Stroke, RoundedCornerShape(16.dp)).padding(15.dp),
    ) {
        Text(label, color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .75.sp)
        Spacer(Modifier.height(5.dp))
        Text(value, color = tint, fontSize = 17.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(3.dp))
        Text(detail, color = Muted, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun Panel(background: Color = Surface, content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(17.dp)).background(background)
            .border(1.dp, Stroke.copy(alpha = .85f), RoundedCornerShape(17.dp)).padding(16.dp),
        content = content,
    )
}

@Composable
private fun Notice(title: String, detail: String, color: Color) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(color.copy(alpha = .09f))
            .border(1.dp, color.copy(alpha = .24f), RoundedCornerShape(14.dp)).padding(14.dp),
        verticalAlignment = Alignment.Top,
    ) {
        StatusDot(color)
        Spacer(Modifier.width(9.dp))
        Column {
            Text(title, color = color, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            if (detail.isNotBlank()) Text(detail, color = Muted, fontSize = 12.sp, lineHeight = 13.sp, maxLines = 5, overflow = TextOverflow.Ellipsis)
        }
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun EmptyPanel(title: String, detail: String) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(15.dp)).background(SurfaceInset)
            .border(1.dp, Stroke.copy(alpha = .8f), RoundedCornerShape(15.dp)).padding(20.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("◇", color = Mint.copy(alpha = .75f), fontSize = 19.sp)
        Spacer(Modifier.height(4.dp))
        Text(title, color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        Text(detail, color = Muted, fontSize = 12.sp, lineHeight = 13.sp)
    }
    Spacer(Modifier.height(7.dp))
}

@Composable
private fun SectionTitle(title: String, detail: String, onClick: (() -> Unit)? = null) {
    Row(
        Modifier.fillMaxWidth().then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier).padding(top = 4.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, Modifier.weight(1f), color = Ink, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        Text(detail.uppercase(), color = Faint, fontSize = 12.sp, letterSpacing = .55.sp)
        if (onClick != null) Text("  ↗", color = Mint, fontSize = 12.sp)
    }
}

@Composable
private fun FooterLine(state: ObservatoryState) {
    Row(
        Modifier.fillMaxWidth().background(Color(0xFF080E12)).padding(horizontal = 18.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StatusDot(if (state.streamConnected) Mint else if (state.liveError == null) Gold else Coral)
        Spacer(Modifier.width(7.dp))
        Text(if (state.streamConnected) "STREAMING" else if (state.liveError != null) "RECONNECTING" else "SYNCING", color = Muted, fontSize = 12.sp, letterSpacing = .8.sp)
        Spacer(Modifier.weight(1f))
        Text("PAPER FIRST  ·  READ ONLY", color = Faint, fontSize = 12.sp, letterSpacing = .7.sp)
    }
}

@Composable
private fun OrbitGraphic(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val center = Offset(size.width / 2f, size.height / 2f)
        val r = size.minDimension * .34f
        drawCircle(Mint.copy(alpha = .08f), r * 1.55f, center)
        drawCircle(Stroke.copy(alpha = .8f), r, center, style = DrawStroke(width = 1.dp.toPx()))
        withTransform({ rotate(-26f, pivot = center) }) {
            drawOval(Color.Transparent, topLeft = Offset(center.x - r * 1.42f, center.y - r * .42f), size = androidx.compose.ui.geometry.Size(r * 2.84f, r * .84f), style = DrawStroke(width = 1.dp.toPx()))
        }
        withTransform({ rotate(34f, pivot = center) }) {
            drawOval(Color.Transparent, topLeft = Offset(center.x - r * 1.42f, center.y - r * .42f), size = androidx.compose.ui.geometry.Size(r * 2.84f, r * .84f), style = DrawStroke(width = 1.dp.toPx()))
        }
        val line = Path().apply {
            moveTo(center.x - r * .8f, center.y + r * .05f)
            lineTo(center.x - r * .3f, center.y + r * .05f)
            lineTo(center.x - r * .04f, center.y - r * .52f)
            lineTo(center.x + r * .2f, center.y + r * .55f)
            lineTo(center.x + r * .45f, center.y - r * .15f)
            lineTo(center.x + r * .78f, center.y - r * .15f)
        }
        drawPath(line, Mint, style = DrawStroke(width = 2.dp.toPx(), cap = androidx.compose.ui.graphics.StrokeCap.Round, join = androidx.compose.ui.graphics.StrokeJoin.Round))
        drawCircle(Aqua, 2.5.dp.toPx(), Offset(center.x + r * 1.1f, center.y - r * .78f))
    }
}

@Composable
private fun StatusDot(color: Color) {
    Box(Modifier.size(7.dp).clip(CircleShape).background(color))
}

@Composable
private fun Pill(text: String, color: Color, background: Color) {
    Box(Modifier.clip(RoundedCornerShape(7.dp)).background(background).padding(horizontal = 7.dp, vertical = 4.dp)) {
        Text(text, color = color, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .45.sp, maxLines = 1)
    }
}

@Composable
private fun Eyebrow(text: String) {
    Text(text, color = Mint.copy(alpha = .85f), fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.05.sp)
    Spacer(Modifier.height(5.dp))
}

@Composable
private fun TinyValue(label: String, value: String) {
    Column(Modifier.widthIn(max = 125.dp)) {
        Text(label, color = Faint, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .45.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(value, color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun Hairline() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(Stroke.copy(alpha = .75f)))
}

@Composable
private fun FieldLabel(text: String) {
    Text(text, color = Muted, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = .8.sp)
    Spacer(Modifier.height(5.dp))
}

@Composable
private fun fieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = Ink,
    unfocusedTextColor = Ink,
    focusedBorderColor = Mint,
    unfocusedBorderColor = Stroke,
    cursorColor = Mint,
    focusedContainerColor = SurfaceInset,
    unfocusedContainerColor = SurfaceInset,
)

@Composable
private fun SettingsDialog(
    current: CockpitCredentials,
    saving: Boolean,
    error: String?,
    onDismiss: () -> Unit,
    onSaveServer: (String, Boolean) -> Unit,
    onLogout: () -> Unit,
) {
    var url by remember(current.serverUrl) { mutableStateOf(current.serverUrl) }
    var privateNetworkServer by remember(current.localNetworkPermissionRequested) {
        mutableStateOf(current.localNetworkPermissionRequested)
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = SurfaceRaised,
        title = { Text("Cockpit connection", color = Ink, fontWeight = FontWeight.Bold) },
        text = {
            Column {
                Text("Signed in as ${current.username}", color = Muted, fontSize = 12.sp)
                Spacer(Modifier.height(12.dp))
                FieldLabel("SERVER URL")
                OutlinedTextField(
                    value = url,
                    onValueChange = { url = it },
                    enabled = !saving,
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    colors = fieldColors(),
                    shape = RoundedCornerShape(12.dp),
                )
                if (Build.VERSION.SDK_INT >= 37) {
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 4.dp)) {
                        Checkbox(
                            checked = privateNetworkServer,
                            onCheckedChange = { privateNetworkServer = it },
                            enabled = !saving,
                            colors = CheckboxDefaults.colors(checkedColor = Mint, uncheckedColor = Stroke, checkmarkColor = Night),
                        )
                        Column {
                            Text("Private-network cockpit", color = Ink, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                            Text("Include internal hostnames", color = Faint, fontSize = 12.sp)
                        }
                    }
                }
                Spacer(Modifier.height(8.dp))
                Text("Remote servers must use HTTPS. Sign-in credentials remain encrypted on this device.", color = Faint, fontSize = 12.sp, lineHeight = 14.sp)
                if (error != null) {
                    Spacer(Modifier.height(8.dp))
                    Text(error, color = Coral, fontSize = 12.sp, lineHeight = 16.sp)
                }
            }
        },
        confirmButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                TextButton(onClick = onLogout, enabled = !saving) { Text("Disconnect", color = Coral) }
                    TextButton(onClick = { onSaveServer(url, privateNetworkServer) }, enabled = !saving) {
                    Text(if (saving) "Connecting…" else "Save", color = Mint, fontWeight = FontWeight.Bold)
                }
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = Muted) } },
    )
}

private fun tabSubtitle(tab: CockpitTab): String = when (tab) {
    CockpitTab.OVERVIEW -> "FLEET SNAPSHOT · LIVE HEALTH · CAPITAL"
    CockpitTab.POSITIONS -> "OPEN RISK · MARKS · ORDER ACTIVITY"
    CockpitTab.TRADE_HISTORY -> "CLOSED TRADES · REALIZED LEDGER"
    CockpitTab.EXECUTION -> "OPEN QUEUE · ROUTE CONVERSION · REJECTS"
    CockpitTab.MARKETS -> "LIVE WINDOWS · BOOK SNAPSHOTS"
    CockpitTab.PREDICTIONS -> "FORECASTS · SOURCE FRESHNESS · MARKET REFERENCE"
    CockpitTab.BOT_FLEET -> "BOT AVAILABILITY · INSTANCE PERFORMANCE"
    CockpitTab.WINDOW_BOTS -> "WINDOW STRATEGIES · ENTRY GATES · EXITS"
    CockpitTab.STRATEGIES -> "CONFIGURED STRATEGIES · RECENT DECISIONS"
    CockpitTab.ENGINE_LAB -> "PREDICTOR CHECKPOINTS · SIGNAL QUALITY"
    CockpitTab.ACTIVITY_LOGS -> "BOT LOGS · SUPERVISOR EVENTS · OPERATIONS"
}

private fun readable(error: Throwable): String = when (error) {
    is CockpitHttpException -> error.message ?: "Cockpit request failed."
    else -> when {
        error.message?.contains("permission denied", true) == true || error.message?.contains("EACCES", true) == true ->
            "Android blocked access to this local cockpit. Reopen connection settings and save the server to grant local-network access."
        else -> error.message?.takeIf { it.isNotBlank() } ?: "Could not reach the cockpit server."
    }
}

private fun money(value: Double?): String {
    if (value == null || !value.isFinite()) return "—"
    return if (value < 0) "−$${String.format(Locale.US, "%,.2f", kotlin.math.abs(value))}" else "$${String.format(Locale.US, "%,.2f", value)}"
}

private fun price(value: Double?): String = value?.takeIf(Double::isFinite)?.let {
    if (it >= 100) String.format(Locale.US, "$%,.2f", it) else String.format(Locale.US, "%.4f", it)
} ?: "—"

private fun probability(value: Double?): String = value?.takeIf(Double::isFinite)?.let {
    String.format(Locale.US, "%.1f%%", if (it in 0.0..1.0) it * 100.0 else it)
} ?: "—"

private fun num(value: Double?, decimals: Int): String = value?.takeIf(Double::isFinite)?.let { String.format(Locale.US, "%.${decimals}f", it) } ?: "—"

private fun duration(value: Double?): String {
    val seconds = value?.toInt() ?: return "—"
    if (seconds <= 0) return "—"
    return if (seconds >= 60) "${seconds / 60}m ${seconds % 60}s" else "${seconds}s"
}

private fun horizon(value: Double?): String = when (value?.toInt()) {
    300 -> "5M"
    900 -> "15M"
    3600 -> "1H"
    86400 -> "1D"
    null, 0 -> "WINDOW"
    else -> "${value.toInt()}S"
}

private fun pnlTint(value: Double?): Color = when {
    value == null || value == 0.0 -> Muted
    value > 0.0 -> Mint
    else -> Coral
}

private fun clock(timestamp: Long): String = runCatching {
    DateTimeFormatter.ofPattern("HH:mm:ss", Locale.US).withZone(ZoneId.systemDefault()).format(Instant.ofEpochMilli(timestamp))
}.getOrDefault("—")

private fun clockValue(value: Any?): String = when (value) {
    is Number -> clock(value.toLong().let { if (it < 10_000_000_000L) it * 1000 else it })
    is String -> value.takeIf { it.isNotBlank() }?.replace('T', ' ')?.take(19) ?: "—"
    else -> "—"
}

private fun JSONObject.double(key: String): Double? {
    if (!has(key) || isNull(key)) return null
    val value = optDouble(key, Double.NaN)
    return value.takeIf(Double::isFinite)
}

private fun firstDouble(json: JSONObject, vararg keys: String): Double? = keys.firstNotNullOfOrNull { json.double(it) }

private fun JSONArray.objects(): List<JSONObject> = buildList {
    for (index in 0 until length()) optJSONObject(index)?.let(::add)
}

private fun JSONArray.strings(): List<String> = buildList {
    for (index in 0 until length()) optString(index).takeIf(String::isNotBlank)?.let(::add)
}

private inline fun JSONArray.forEachString(action: (String) -> Unit) = strings().forEach(action)
private inline fun JSONArray.forEachObject(action: (JSONObject) -> Unit) = objects().forEach(action)
