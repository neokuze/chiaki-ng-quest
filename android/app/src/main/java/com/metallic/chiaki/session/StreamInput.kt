package com.metallic.chiaki.session

import android.content.Context
import android.hardware.*
import android.view.*
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.OnLifecycleEvent
import com.metallic.chiaki.common.Preferences
import com.metallic.chiaki.lib.ControllerState

/** What the touchpad overlay shows: hold hint [progress] 0..1 or -1, fingers packed as x | y << 16 or -1 */
data class TouchOverlay(val progress: Float, val active: Boolean, val finger0: Int, val finger1: Int)
{
	companion object
	{
		val NONE = TouchOverlay(-1f, false, -1, -1)
	}
}

class StreamInput(val context: Context, val preferences: Preferences)
{
	var controllerStateChangedCallback: ((ControllerState) -> Unit)? = null

	val controllerState: ControllerState get()
	{
		val controllerState = sensorControllerState or keyControllerState or motionControllerState

		val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
		@Suppress("DEPRECATION")
		when(windowManager.defaultDisplay.rotation)
		{
			Surface.ROTATION_90 -> {
				controllerState.accelX *= -1.0f
				controllerState.accelZ *= -1.0f
				controllerState.gyroX *= -1.0f
				controllerState.gyroZ *= -1.0f
				controllerState.orientX *= -1.0f
				controllerState.orientZ *= -1.0f
			}
			else -> {}
		}

		// prioritize motion controller's l2 and r2 over key
		// (some controllers send only key, others both but key earlier than full press)
		if(motionControllerState.l2State > 0U)
			controllerState.l2State = motionControllerState.l2State
		if(motionControllerState.r2State > 0U)
			controllerState.r2State = motionControllerState.r2State

		val state = controllerState or touchControllerState or virtualTouchState
		if(shaking)
		{
			// Side to side shake, the way a hand shakes a controller
			val t = (android.os.SystemClock.elapsedRealtime() - shakeStart) / 1000f
			val phase = 2f * Math.PI.toFloat() * SHAKE_HZ * t
			state.accelX = kotlin.math.sin(phase) * SHAKE_ACCEL_G
			state.accelY = 1f
			state.accelZ = 0f
			state.gyroX = 0f
			state.gyroY = 0f
			state.gyroZ = kotlin.math.cos(phase) * SHAKE_GYRO
		}
		else motionOverride?.let {
			state.gyroX = it[0]; state.gyroY = it[1]; state.gyroZ = it[2]
			state.accelX = it[3]; state.accelY = it[4]; state.accelZ = it[5]
			state.orientX = it[6]; state.orientY = it[7]; state.orientZ = it[8]; state.orientW = it[9]
		}
		return state
	}

	/**
	 * Motion from somewhere else than the device sensors, e.g. a VR controller:
	 * gyro xyz (rad/s), accel xyz (g), orientation xyzw
	 */
	var motionOverride: FloatArray? = null
		set(value)
		{
			field = value
			controllerStateUpdated()
		}

	private var shaking = false
	private var shakeStart = 0L
	private val shakeTicker = object: Runnable {
		override fun run()
		{
			if(!shaking)
				return
			controllerStateUpdated()
			handler.postDelayed(this, 8)
		}
	}

	/** Shakes the virtual controller for as long as [on] stays true */
	fun setShaking(on: Boolean)
	{
		if(on == shaking)
			return
		shaking = on
		handler.removeCallbacks(shakeTicker)
		if(on)
		{
			shakeStart = android.os.SystemClock.elapsedRealtime()
			handler.post(shakeTicker)
		}
		else
			controllerStateUpdated()
	}

	private val sensorControllerState = ControllerState() // from Motion Sensors
	private val keyControllerState = ControllerState() // from KeyEvents
	private val motionControllerState = ControllerState() // from MotionEvents
	var touchControllerState = ControllerState()
		set(value)
		{
			field = value
			controllerStateUpdated()
		}

	private val swapCrossMoon = preferences.swapCrossMoon

	private val sensorEventListener = object: SensorEventListener {
		override fun onSensorChanged(event: SensorEvent)
		{
			when(event.sensor.type)
			{
				Sensor.TYPE_ACCELEROMETER -> {
					sensorControllerState.accelX = event.values[1] / SensorManager.GRAVITY_EARTH
					sensorControllerState.accelY = event.values[2] / SensorManager.GRAVITY_EARTH
					sensorControllerState.accelZ = event.values[0] / SensorManager.GRAVITY_EARTH
				}
				Sensor.TYPE_GYROSCOPE -> {
					sensorControllerState.gyroX = event.values[1]
					sensorControllerState.gyroY = event.values[2]
					sensorControllerState.gyroZ = event.values[0]
				}
				Sensor.TYPE_ROTATION_VECTOR -> {
					val q = floatArrayOf(0f, 0f, 0f, 0f)
					SensorManager.getQuaternionFromVector(q, event.values)
					sensorControllerState.orientX = q[2]
					sensorControllerState.orientY = q[3]
					sensorControllerState.orientZ = q[1]
					sensorControllerState.orientW = q[0]
				}
				else -> return
			}
			controllerStateUpdated()
		}

		override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}
	}

	private val motionLifecycleObserver = object: LifecycleObserver {
		@OnLifecycleEvent(Lifecycle.Event.ON_RESUME)
		fun onResume()
		{
			val samplingPeriodUs = 4000
			val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
			listOfNotNull(
				sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER),
				sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE),
				sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
			).forEach {
				sensorManager.registerListener(sensorEventListener, it, samplingPeriodUs)
			}
		}

		@OnLifecycleEvent(Lifecycle.Event.ON_PAUSE)
		fun onPause()
		{
			val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
			sensorManager.unregisterListener(sensorEventListener)
		}
	}

	fun observe(lifecycleOwner: LifecycleOwner)
	{
		if(preferences.motionEnabled)
			lifecycleOwner.lifecycle.addObserver(motionLifecycleObserver)
	}

	private fun controllerStateUpdated()
	{
		controllerStateChangedCallback?.let { it(controllerState) }
	}

	private fun setKeyButton(mask: UInt, pressed: Boolean)
	{
		keyControllerState.buttons = if(pressed) keyControllerState.buttons or mask else keyControllerState.buttons and mask.inv()
		controllerStateUpdated()
	}

	// Options is a modifier: Options + R3 = touchpad click, Options alone is sent as a tap on release
	private var optionsHeld = false
	private var optionsCombo = false
	private val handler = android.os.Handler(android.os.Looper.getMainLooper())

	// --- Touchpad mode: hold Options alone, the right stick is a finger, R3 clicks ---
	var touchOverlayCallback: ((TouchOverlay) -> Unit)? = null
	private var touchMode = false
	private var shakeKey = false
	private var holdClean = false
	private var optionsDownTime = 0L
	private val virtualTouchState = ControllerState()
	private val fingerIds = arrayOfNulls<UByte>(2)

	private val holdTicker = object: Runnable {
		override fun run()
		{
			if(!optionsHeld || !holdClean || touchMode)
				return
			val held = android.os.SystemClock.elapsedRealtime() - optionsDownTime
			if(held >= TOUCH_HOLD_MS)
			{
				touchMode = true
				optionsCombo = true
				publishTouchOverlay()
				return
			}
			if(held >= TOUCH_HINT_MS)
				touchOverlayCallback?.invoke(TouchOverlay((held - TOUCH_HINT_MS).toFloat() / (TOUCH_HOLD_MS - TOUCH_HINT_MS), false, -1, -1))
			handler.postDelayed(this, 33)
		}
	}

	private fun cancelHold()
	{
		if(!holdClean)
			return
		holdClean = false
		handler.removeCallbacks(holdTicker)
		touchOverlayCallback?.invoke(TouchOverlay.NONE)
	}

	private fun endTouchMode()
	{
		touchMode = false
		for(i in 0..1)
			setFinger(i, null, null)
		touchOverlayCallback?.invoke(TouchOverlay.NONE)
		controllerStateUpdated()
	}

	private fun setFinger(i: Int, x: Float?, y: Float?)
	{
		val id = fingerIds[i]
		if(x == null || y == null)
		{
			if(id != null)
				virtualTouchState.stopTouch(id)
			fingerIds[i] = null
			return
		}
		val tx = (x.coerceIn(0f, 1f) * (ControllerState.TOUCHPAD_WIDTH - 1U).toFloat()).toUInt().toUShort()
		val ty = (y.coerceIn(0f, 1f) * (ControllerState.TOUCHPAD_HEIGHT - 1U).toFloat()).toUInt().toUShort()
		if(id == null)
			fingerIds[i] = virtualTouchState.startTouch(tx, ty)
		else
			virtualTouchState.setTouchPos(id, tx, ty)
	}

	private fun packedFinger(i: Int): Int
	{
		val id = fingerIds[i] ?: return -1
		val t = virtualTouchState.touches.find { it.id == id.toByte() } ?: return -1
		return t.x.toInt() or (t.y.toInt() shl 16)
	}

	private fun publishTouchOverlay()
	{
		touchOverlayCallback?.invoke(TouchOverlay(-1f, true, packedFinger(0), packedFinger(1)))
	}

	/** A stick deflected past the dead zone is a finger down at that spot of the touchpad */
	private fun stickFinger(i: Int, sx: Float, sy: Float)
	{
		if(sx * sx + sy * sy < STICK_TOUCH_DEADZONE * STICK_TOUCH_DEADZONE)
			setFinger(i, null, null)
		else
			setFinger(i, (sx + 1f) * 0.5f, (sy + 1f) * 0.5f)
	}

	companion object
	{
		private const val TOUCH_HOLD_MS = 2800L
		private const val TOUCH_HINT_MS = 400L
		private const val STICK_TOUCH_DEADZONE = 0.25f
		private const val SHAKE_HZ = 7f
		private const val SHAKE_ACCEL_G = 2.5f
		private const val SHAKE_GYRO = 8f // rad/s
	}

	private fun handleTouchpadCombo(event: KeyEvent): Boolean
	{
		val down = event.action == KeyEvent.ACTION_DOWN
		when(event.keyCode)
		{
			KeyEvent.KEYCODE_MEDIA_RECORD -> { // Share button on Xbox Series controllers
				setKeyButton(ControllerState.BUTTON_TOUCHPAD, down)
				return true
			}
			KeyEvent.KEYCODE_BUTTON_START -> {
				if(event.repeatCount > 0)
					return true
				if(down)
				{
					optionsHeld = true
					optionsCombo = false
					holdClean = true
					optionsDownTime = android.os.SystemClock.elapsedRealtime()
					handler.removeCallbacks(holdTicker)
					handler.postDelayed(holdTicker, 33)
				}
				else
				{
					optionsHeld = false
					cancelHold()
					if(touchMode)
						endTouchMode()
					if(!optionsCombo)
					{
						setKeyButton(ControllerState.BUTTON_OPTIONS, true)
						handler.postDelayed({ setKeyButton(ControllerState.BUTTON_OPTIONS, false) }, 100)
					}
				}
				return true
			}
			KeyEvent.KEYCODE_BUTTON_THUMBL -> {
				// Options + L3 shakes the controller while held
				if(down && optionsHeld && !touchMode)
				{
					if(event.repeatCount == 0)
					{
						cancelHold()
						optionsCombo = true
						shakeKey = true
						setShaking(true)
					}
					return true
				}
				if(!down && shakeKey)
				{
					shakeKey = false
					setShaking(false)
					return true
				}
			}
			KeyEvent.KEYCODE_BUTTON_THUMBR -> {
				if(down && optionsHeld)
				{
					cancelHold()
					optionsCombo = true
					setKeyButton(ControllerState.BUTTON_TOUCHPAD, true)
					return true
				}
				if(!down && keyControllerState.buttons and ControllerState.BUTTON_TOUCHPAD != 0U)
				{
					setKeyButton(ControllerState.BUTTON_TOUCHPAD or ControllerState.BUTTON_R3, false)
					return true
				}
			}
		}
		return false
	}

	fun dispatchKeyEvent(event: KeyEvent): Boolean
	{
		//Log.i("StreamSession", "key event $event")
		if(event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP)
			return false

		if(handleTouchpadCombo(event))
			return true
		if(touchMode)
			return true // only R3 and Options count while the sticks are fingers
		if(event.action == KeyEvent.ACTION_DOWN)
			cancelHold()

		when(event.keyCode)
		{
			KeyEvent.KEYCODE_BUTTON_L2 -> {
				keyControllerState.l2State = if(event.action == KeyEvent.ACTION_DOWN) UByte.MAX_VALUE else 0U
				return true
			}
			KeyEvent.KEYCODE_BUTTON_R2 -> {
				keyControllerState.r2State = if(event.action == KeyEvent.ACTION_DOWN) UByte.MAX_VALUE else 0U
				return true
			}
		}

		val buttonMask: UInt = when(event.keyCode)
		{
			// dpad handled by MotionEvents
			//KeyEvent.KEYCODE_DPAD_LEFT -> ControllerState.BUTTON_DPAD_LEFT
			//KeyEvent.KEYCODE_DPAD_RIGHT -> ControllerState.BUTTON_DPAD_RIGHT
			//KeyEvent.KEYCODE_DPAD_UP -> ControllerState.BUTTON_DPAD_UP
			//KeyEvent.KEYCODE_DPAD_DOWN -> ControllerState.BUTTON_DPAD_DOWN
			KeyEvent.KEYCODE_BUTTON_A -> if(swapCrossMoon) ControllerState.BUTTON_MOON else ControllerState.BUTTON_CROSS
			KeyEvent.KEYCODE_BUTTON_B -> if(swapCrossMoon) ControllerState.BUTTON_CROSS else ControllerState.BUTTON_MOON
			KeyEvent.KEYCODE_BUTTON_X -> if(swapCrossMoon) ControllerState.BUTTON_PYRAMID else ControllerState.BUTTON_BOX
			KeyEvent.KEYCODE_BUTTON_Y -> if(swapCrossMoon) ControllerState.BUTTON_BOX else ControllerState.BUTTON_PYRAMID
			KeyEvent.KEYCODE_BUTTON_L1 -> ControllerState.BUTTON_L1
			KeyEvent.KEYCODE_BUTTON_R1 -> ControllerState.BUTTON_R1
			KeyEvent.KEYCODE_BUTTON_THUMBL -> ControllerState.BUTTON_L3
			KeyEvent.KEYCODE_BUTTON_THUMBR -> ControllerState.BUTTON_R3
			KeyEvent.KEYCODE_BUTTON_SELECT -> ControllerState.BUTTON_SHARE
			KeyEvent.KEYCODE_BUTTON_START -> ControllerState.BUTTON_OPTIONS
			KeyEvent.KEYCODE_BUTTON_C -> ControllerState.BUTTON_PS
			KeyEvent.KEYCODE_BUTTON_MODE -> ControllerState.BUTTON_PS
			else -> return false
		}

		keyControllerState.buttons = keyControllerState.buttons.run {
			when(event.action)
			{
				KeyEvent.ACTION_DOWN -> this or buttonMask
				KeyEvent.ACTION_UP -> this and buttonMask.inv()
				else -> this
			}
		}

		controllerStateUpdated()
		return true
	}

	fun onGenericMotionEvent(event: MotionEvent): Boolean
	{
		if(event.source and InputDevice.SOURCE_CLASS_JOYSTICK != InputDevice.SOURCE_CLASS_JOYSTICK)
			return false
		fun Float.signedAxis() = (this * Short.MAX_VALUE).toInt().toShort()
		fun Float.unsignedAxis() = (this * UByte.MAX_VALUE.toFloat()).toUInt().toUByte()
		if(touchMode)
		{
			stickFinger(0, event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ))
			stickFinger(1, event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y))
			publishTouchOverlay()
			controllerStateUpdated()
			return true
		}
		if(holdClean && listOf(MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
				MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y)
				.any { kotlin.math.abs(event.getAxisValue(it)) > 0.3f })
			cancelHold()
		motionControllerState.leftX = event.getAxisValue(MotionEvent.AXIS_X).signedAxis()
		motionControllerState.leftY = event.getAxisValue(MotionEvent.AXIS_Y).signedAxis()
		motionControllerState.rightX = event.getAxisValue(MotionEvent.AXIS_Z).signedAxis()
		motionControllerState.rightY = event.getAxisValue(MotionEvent.AXIS_RZ).signedAxis()
		motionControllerState.l2State = event.getAxisValue(MotionEvent.AXIS_LTRIGGER).unsignedAxis()
		motionControllerState.r2State = event.getAxisValue(MotionEvent.AXIS_RTRIGGER).unsignedAxis()
		motionControllerState.buttons = motionControllerState.buttons.let {
			val dpadX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
			val dpadY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
			val dpadButtons =
				(if(dpadX > 0.5f) ControllerState.BUTTON_DPAD_RIGHT else 0U) or
						(if(dpadX < -0.5f) ControllerState.BUTTON_DPAD_LEFT else 0U) or
						(if(dpadY > 0.5f) ControllerState.BUTTON_DPAD_DOWN else 0U) or
						(if(dpadY < -0.5f) ControllerState.BUTTON_DPAD_UP else 0U)
			it and (ControllerState.BUTTON_DPAD_RIGHT or
					ControllerState.BUTTON_DPAD_LEFT or
					ControllerState.BUTTON_DPAD_DOWN or
					ControllerState.BUTTON_DPAD_UP).inv() or
					dpadButtons
		}
		//Log.i("StreamSession", "motionEvent => $motionControllerState")
		controllerStateUpdated()
		return true
	}
}