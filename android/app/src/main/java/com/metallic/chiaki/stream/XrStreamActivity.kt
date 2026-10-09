// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.core.content.IntentCompat
import androidx.lifecycle.Observer
import androidx.lifecycle.ViewModelProvider
import com.metallic.chiaki.common.ext.viewModelFactory
import com.metallic.chiaki.lib.ConnectInfo
import com.metallic.chiaki.session.StreamStateCreateError
import com.metallic.chiaki.session.StreamStateLoginPinRequest
import com.metallic.chiaki.session.StreamStateQuit

/**
 * Immersive stream: the video is shown on a virtual screen through OpenXR and the
 * Quest Touch controllers act as a DualSense. Physical gamepads keep working too.
 */
class XrStreamActivity : ComponentActivity()
{
	companion object
	{
		private const val TAG = "XrStreamActivity"
	}

	private lateinit var viewModel: StreamViewModel
	private var renderer: XrRenderer? = null

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)

		val connectInfo = IntentCompat.getParcelableExtra(intent, StreamActivity.EXTRA_CONNECT_INFO, ConnectInfo::class.java)
		if(connectInfo == null)
		{
			finish()
			return
		}

		viewModel = ViewModelProvider(this, viewModelFactory {
			StreamViewModel(application, connectInfo)
		})[StreamViewModel::class.java]
		viewModel.input.observe(this)

		viewModel.session.state.observe(this, Observer {
			when(it)
			{
				is StreamStateQuit -> {
					Log.i(TAG, "Session quit: ${it.reason} ${it.reasonString ?: ""}")
					finish()
				}
				is StreamStateCreateError -> {
					Log.e(TAG, "Session create error: ${it.error.errorCode}")
					finish()
				}
				is StreamStateLoginPinRequest -> {
					// TODO: PIN entry is not available in VR yet
					Log.e(TAG, "Console requires a login PIN, not supported in immersive mode")
					finish()
				}
				else -> {}
			}
		})

		val profile = connectInfo.videoProfile
		renderer = XrRenderer(this,
			surfaceCallback = { viewModel.session.setExternalSurface(it) },
			inputCallback = { viewModel.input.touchControllerState = it },
			exitCallback = { finish() }
		).also { it.start(profile.width, profile.height) }
	}

	override fun onResume()
	{
		super.onResume()
		viewModel.session.resume()
	}

	override fun onPause()
	{
		super.onPause()
		viewModel.session.pause()
	}

	override fun onDestroy()
	{
		super.onDestroy()
		renderer?.stop()
		renderer = null
		if(isFinishing && ::viewModel.isInitialized)
			viewModel.session.shutdown()
	}

	override fun dispatchKeyEvent(event: KeyEvent) =
		(::viewModel.isInitialized && viewModel.input.dispatchKeyEvent(event)) || super.dispatchKeyEvent(event)

	override fun onGenericMotionEvent(event: MotionEvent) =
		(::viewModel.isInitialized && viewModel.input.onGenericMotionEvent(event)) || super.onGenericMotionEvent(event)
}
