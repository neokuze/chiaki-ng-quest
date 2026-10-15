// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.session

import android.content.Context
import android.graphics.SurfaceTexture
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.metallic.chiaki.common.Preferences
import android.util.Log
import android.view.*
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import com.metallic.chiaki.common.LogManager
import com.metallic.chiaki.lib.*

sealed class StreamState
object StreamStateIdle: StreamState()
object StreamStateConnecting: StreamState()
object StreamStateConnected: StreamState()
data class StreamStateCreateError(val error: CreateError): StreamState()
data class StreamStateQuit(val reason: QuitReason, val reasonString: String?): StreamState()
data class StreamStateLoginPinRequest(val pinIncorrect: Boolean): StreamState()

class StreamSession(connectInfo: ConnectInfo, val logManager: LogManager, val logVerbose: Boolean, val input: StreamInput)
{
	var connectInfo = connectInfo
		private set

	@Volatile var session: Session? = null
		private set

	/** Short message to show over the stream, e.g. while Bad Network Mode switches resolution */
	private val _notice = MutableLiveData<String?>(null)
	val notice: LiveData<String?> get() = _notice

	private val _state = MutableLiveData<StreamState>(StreamStateIdle)
	val state: LiveData<StreamState> get() = _state
	private val _rumbleState = MutableLiveData<RumbleEvent>(RumbleEvent(0U, 0U))
	val rumbleState: LiveData<RumbleEvent> get() = _rumbleState

	private var surfaceTexture: SurfaceTexture? = null
	private var surface: Surface? = null

	// Without this the WiFi radio dozes while the device mostly receives, which stalls the
	// video until something is sent again (e.g. moving a stick), and it also pauses for scans
	@Suppress("DEPRECATION")
	private val wifiLock: WifiManager.WifiLock? = (input.context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager)
		?.createWifiLock(
			if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF,
			"chiaki:stream")
		?.apply { setReferenceCounted(false) }

	init
	{
		input.controllerStateChangedCallback = {
			if(it.buttons != 0U || it.l2State > 16U || it.r2State > 16U
					|| it.leftX.absBig() || it.leftY.absBig() || it.rightX.absBig() || it.rightY.absBig())
				lastPlayInput = SystemClock.elapsedRealtime()
			session?.setControllerState(it)
		}
	}

	private fun Short.absBig() = this > 6000 || this < -6000

	// --- Bad Network Mode ---
	// The console fixes the resolution when the session starts, so changing it means
	// reconnecting. Going down only happens once the picture is already frozen, going
	// back up only while nobody is playing, so the switch never costs a live moment.

	private val badNetworkMode = input.preferences.badNetworkMode
	private val handler = Handler(Looper.getMainLooper())
	/** Resolutions to walk through, best first, starting with the one from the settings */
	private val steps = Preferences.Resolution.values()
		.filter { it.preset.value <= input.preferences.resolution.preset.value }
		.sortedByDescending { it.preset.value }
	private var step = 0
	private var reconnecting = false
	private var lastFrames = -1L
	private var frozenMs = 0L
	private var stableMs = 0L
	@Volatile private var lastPlayInput = 0L
	@Volatile private var retired: Session? = null

	private val monitor = object: Runnable {
		override fun run()
		{
			handler.postDelayed(this, MONITOR_INTERVAL_MS)
			val session = session
			if(session == null || paused || reconnecting || _state.value != StreamStateConnected)
			{
				lastFrames = -1L
				frozenMs = 0L
				return
			}
			val frames = session.framesDecoded()
			if(lastFrames >= 0 && frames == lastFrames)
			{
				frozenMs += MONITOR_INTERVAL_MS
				stableMs = 0L
			}
			else
			{
				frozenMs = 0L
				stableMs += MONITOR_INTERVAL_MS
			}
			lastFrames = frames

			if(frozenMs >= FREEZE_DOWNGRADE_MS && step < steps.size - 1)
			{
				Log.i("StreamSession", "Bad Network Mode: frozen for ${frozenMs}ms, going down to ${steps[step + 1].value}")
				reconnect(step + 1)
			}
			else if(step > 0 && stableMs >= STABLE_UPGRADE_MS
					&& SystemClock.elapsedRealtime() - lastPlayInput >= IDLE_UPGRADE_MS)
			{
				Log.i("StreamSession", "Bad Network Mode: stable for ${stableMs}ms and idle, going up to ${steps[step - 1].value}")
				reconnect(step - 1)
			}
		}
	}

	private fun reconnect(newStep: Int)
	{
		reconnecting = true
		step = newStep
		frozenMs = 0L
		stableMs = 0L
		lastFrames = -1L
		val res = steps[step]
		val prefs = input.preferences
		val preset = ConnectVideoProfile.preset(res.preset, prefs.fps.preset, prefs.codec.codec)
		val userBitrate = prefs.bitrate
		val profile = if(userBitrate != null && userBitrate < preset.bitrate) preset.copy(bitrate = userBitrate) else preset
		connectInfo = connectInfo.copy(videoProfile = profile)
		_notice.value = input.context.getString(com.metallic.chiaki.R.string.stream_adjusting_quality, res.value)

		val old = session
		retired = old
		session = null
		// Stopping joins the session threads, keep that off the main thread
		Thread {
			old?.stop()
			old?.dispose()
			handler.post {
				if(reconnecting && session == null)
					startSession(silent = true)
			}
		}.start()
	}

	fun shutdown()
	{
		handler.removeCallbacks(monitor)
		reconnecting = false
		wifiLock?.release()
		session?.stop()
		session?.dispose()
		session = null
		_state.value = StreamStateIdle
		//surfaceTexture?.release()
	}

	private var paused = false

	fun pause()
	{
		// Keep the session alive while paused (e.g. headset taken off on Quest) so the
		// console doesn't drop it; shutdown happens when the ViewModel is cleared.
		paused = true
	}

	fun resume()
	{
		paused = false
		wifiLock?.acquire()
		if(session != null || reconnecting)
			return
		if(badNetworkMode)
		{
			handler.removeCallbacks(monitor)
			handler.postDelayed(monitor, MONITOR_INTERVAL_MS)
		}
		startSession(silent = false)
	}

	private fun startSession(silent: Boolean)
	{
		try
		{
			val session = Session(connectInfo, logManager.createNewFile().file.absolutePath, logVerbose)
			if(!silent)
				_state.value = StreamStateConnecting
			session.eventCallback = { event -> eventCallback(session, event) }
			session.start()
			val surface = surface
			if(surface != null)
				session.setSurface(surface)
			this.session = session
		}
		catch(e: CreateError)
		{
			reconnecting = false
			_notice.value = null
			_state.value = StreamStateCreateError(e)
		}
	}

	companion object
	{
		private const val MONITOR_INTERVAL_MS = 500L
		private const val FREEZE_DOWNGRADE_MS = 3000L
		private const val STABLE_UPGRADE_MS = 120_000L
		private const val IDLE_UPGRADE_MS = 10_000L
		private const val NOTICE_LINGER_MS = 1500L
	}

	private fun eventCallback(source: Session, event: Event)
	{
		// Late events (e.g. its quit) from a session replaced by Bad Network Mode
		if(source === retired)
			return
		when(event)
		{
			is ConnectedEvent -> {
				_state.postValue(StreamStateConnected)
				if(reconnecting)
					handler.post {
						reconnecting = false
						handler.postDelayed({ if(!reconnecting) _notice.value = null }, NOTICE_LINGER_MS)
					}
			}
			is QuitEvent -> _state.postValue(
				StreamStateQuit(
					event.reason,
					event.reasonString
				)
			)
			is LoginPinRequestEvent -> _state.postValue(
				StreamStateLoginPinRequest(
					event.pinIncorrect
				)
			)
			is RumbleEvent -> _rumbleState.postValue(event)
		}
	}

	fun attachToSurfaceView(surfaceView: SurfaceView)
	{
		surfaceView.holder.addCallback(object: SurfaceHolder.Callback {
			override fun surfaceCreated(holder: SurfaceHolder) { }

			override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int)
			{
				val surface = holder.surface
				this@StreamSession.surface = surface
				session?.setSurface(surface)
			}

			override fun surfaceDestroyed(holder: SurfaceHolder)
			{
				this@StreamSession.surface = null
				session?.setSurface(null)
			}
		})
	}

	fun attachToTextureView(textureView: TextureView)
	{
		textureView.surfaceTextureListener = object: TextureView.SurfaceTextureListener {
			override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int)
			{
				if(surfaceTexture != null)
					return
				surfaceTexture = surface
				this@StreamSession.surface = Surface(surfaceTexture)
				session?.setSurface(Surface(surface))
			}

			override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean
			{
				// return false if we want to keep the surface texture
				return surfaceTexture == null
			}

			override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) { }
			override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}
		}

		val surfaceTexture = surfaceTexture
		if(surfaceTexture != null)
			textureView.setSurfaceTexture(surfaceTexture)
	}

	fun setLoginPin(pin: String)
	{
		session?.setLoginPin(pin)
	}
}