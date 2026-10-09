// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// Immersive Quest mode: an OpenXR session whose only content is a quad layer backed by an
// Android surface swapchain. The video decoder renders straight into that surface, so the
// compositor shows the stream on a virtual screen without any GL drawing of our own.
// Quest Touch controllers are read through OpenXR actions and reported as a DualSense state.

#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "ChiakiXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Bits match ControllerState in Chiaki.kt
#define BTN_CROSS    (1 << 0)
#define BTN_MOON     (1 << 1)
#define BTN_BOX      (1 << 2)
#define BTN_PYRAMID  (1 << 3)
#define BTN_DPAD_LEFT  (1 << 4)
#define BTN_DPAD_RIGHT (1 << 5)
#define BTN_DPAD_UP    (1 << 6)
#define BTN_DPAD_DOWN  (1 << 7)
#define BTN_L1       (1 << 8)
#define BTN_R1       (1 << 9)
#define BTN_L3       (1 << 10)
#define BTN_R3       (1 << 11)
#define BTN_OPTIONS  (1 << 12)
#define BTN_SHARE    (1 << 13)
#define BTN_PS       (1 << 15)

#define DPAD_THRESHOLD 0.5f
#define OPTIONS_TAP_FRAMES 8 // ~90 ms at 90 Hz, long enough for the console to see the press

#define SCREEN_WIDTH_M 3.2f
#define SCREEN_DISTANCE_M 2.4f

enum {
	A_CROSS, A_MOON, A_BOX, A_PYRAMID, A_L3, A_R3, A_MENU,
	A_L2, A_R2, A_L1, A_R1, A_LSTICK, A_RSTICK, A_COUNT
};

typedef struct XrRenderer
{
	JavaVM *vm;
	jobject activity; // global ref
	jobject callback; // global ref, the Kotlin XrRenderer
	jmethodID on_surface, on_input, on_exit;
	int width, height;

	pthread_t thread;
	volatile bool quit;

	EGLDisplay egl_display;
	EGLConfig egl_config;
	EGLContext egl_context;
	EGLSurface egl_surface;

	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace space;
	XrSwapchain swapchain;
	XrSessionState state;
	bool running;

	XrActionSet action_set;
	XrAction actions[A_COUNT];
	bool menu_held;
	bool menu_combo;
	int options_frames;
} XrRenderer;

#define XR_CHECK(expr) do { XrResult _r = (expr); if(XR_FAILED(_r)) { LOGE("%s failed: %d", #expr, (int)_r); goto fail; } } while(0)

static bool egl_init(XrRenderer *r)
{
	r->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if(!eglInitialize(r->egl_display, NULL, NULL))
		return false;
	const EGLint cfg_attr[] = {
		EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint n = 0;
	if(!eglChooseConfig(r->egl_display, cfg_attr, &r->egl_config, 1, &n) || n < 1)
		return false;
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	r->egl_context = eglCreateContext(r->egl_display, r->egl_config, EGL_NO_CONTEXT, ctx_attr);
	if(r->egl_context == EGL_NO_CONTEXT)
		return false;
	const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	r->egl_surface = eglCreatePbufferSurface(r->egl_display, r->egl_config, pb_attr);
	return eglMakeCurrent(r->egl_display, r->egl_surface, r->egl_surface, r->egl_context);
}

static void egl_fini(XrRenderer *r)
{
	if(r->egl_display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(r->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if(r->egl_surface != EGL_NO_SURFACE)
		eglDestroySurface(r->egl_display, r->egl_surface);
	if(r->egl_context != EGL_NO_CONTEXT)
		eglDestroyContext(r->egl_display, r->egl_context);
	eglTerminate(r->egl_display);
}

static XrPath path(XrRenderer *r, const char *s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(r->instance, s, &p);
	return p;
}

static bool create_action(XrRenderer *r, int idx, const char *name, XrActionType type)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, sizeof(info.actionName) - 1);
	strncpy(info.localizedActionName, name, sizeof(info.localizedActionName) - 1);
	return XR_SUCCEEDED(xrCreateAction(r->action_set, &info, &r->actions[idx]));
}

static bool input_init(XrRenderer *r)
{
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(set_info.actionSetName, "gamepad");
	strcpy(set_info.localizedActionSetName, "Gamepad");
	if(XR_FAILED(xrCreateActionSet(r->instance, &set_info, &r->action_set)))
		return false;

	static const struct { int idx; const char *name; XrActionType type; const char *binding; } map[] = {
		{ A_CROSS,   "cross",   XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click" },
		{ A_MOON,    "moon",    XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click" },
		{ A_BOX,     "box",     XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click" },
		{ A_PYRAMID, "pyramid", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click" },
		{ A_L3,      "l3",      XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click" },
		{ A_R3,      "r3",      XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click" },
		{ A_MENU,    "menu",    XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click" },
		{ A_L2,      "l2",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/left/input/trigger/value" },
		{ A_R2,      "r2",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/right/input/trigger/value" },
		{ A_L1,      "l1",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/left/input/squeeze/value" },
		{ A_R1,      "r1",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/right/input/squeeze/value" },
		{ A_LSTICK,  "lstick",  XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick" },
		{ A_RSTICK,  "rstick",  XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick" },
	};

	XrActionSuggestedBinding bindings[A_COUNT];
	for(int i = 0; i < A_COUNT; i++)
	{
		if(!create_action(r, map[i].idx, map[i].name, map[i].type))
			return false;
		bindings[i].action = r->actions[map[i].idx];
		bindings[i].binding = path(r, map[i].binding);
	}

	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = path(r, "/interaction_profiles/oculus/touch_controller");
	suggested.suggestedBindings = bindings;
	suggested.countSuggestedBindings = A_COUNT;
	if(XR_FAILED(xrSuggestInteractionProfileBindings(r->instance, &suggested)))
		return false;

	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &r->action_set;
	return XR_SUCCEEDED(xrAttachSessionActionSets(r->session, &attach));
}

static bool get_bool(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(r->session, &gi, &s)) && s.isActive && s.currentState;
}

static float get_float(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
	return (XR_SUCCEEDED(xrGetActionStateFloat(r->session, &gi, &s)) && s.isActive) ? s.currentState : 0.0f;
}

static XrVector2f get_vec2(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrVector2f zero = { 0, 0 };
	return (XR_SUCCEEDED(xrGetActionStateVector2f(r->session, &gi, &s)) && s.isActive) ? s.currentState : zero;
}

static jshort stick_axis(float v)
{
	if(v > 1.0f) v = 1.0f;
	if(v < -1.0f) v = -1.0f;
	return (jshort)(v * 32767.0f);
}

static void poll_input(XrRenderer *r, JNIEnv *env)
{
	jint buttons = 0;
	jint l2 = 0, r2 = 0;
	jshort lx = 0, ly = 0, rx = 0, ry = 0;

	if(r->state == XR_SESSION_STATE_FOCUSED)
	{
		XrActiveActionSet active = { r->action_set, XR_NULL_PATH };
		XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		if(xrSyncActions(r->session, &sync) == XR_SUCCESS)
		{
			bool menu = get_bool(r, A_MENU);
			bool cross = get_bool(r, A_CROSS);
			bool moon = get_bool(r, A_MOON);
			XrVector2f ls = get_vec2(r, A_LSTICK);
			XrVector2f rs = get_vec2(r, A_RSTICK);

			// Menu is a modifier: Menu + A = PS, Menu + B = Share, Menu + left stick = D-pad.
			// Options is only sent as a tap when Menu is released without any combo.
			if(menu)
			{
				if(!r->menu_held)
					r->menu_combo = false;
				if(cross) { buttons |= BTN_PS; r->menu_combo = true; }
				if(moon) { buttons |= BTN_SHARE; r->menu_combo = true; }
				if(ls.y > DPAD_THRESHOLD) { buttons |= BTN_DPAD_UP; r->menu_combo = true; }
				if(ls.y < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_DOWN; r->menu_combo = true; }
				if(ls.x < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_LEFT; r->menu_combo = true; }
				if(ls.x > DPAD_THRESHOLD) { buttons |= BTN_DPAD_RIGHT; r->menu_combo = true; }
				ls.x = ls.y = 0.0f; // the stick is the D-pad while Menu is held
			}
			else
			{
				if(r->menu_held && !r->menu_combo)
					r->options_frames = OPTIONS_TAP_FRAMES;
				if(cross) buttons |= BTN_CROSS;
				if(moon) buttons |= BTN_MOON;
			}
			r->menu_held = menu;
			if(r->options_frames > 0)
			{
				buttons |= BTN_OPTIONS;
				r->options_frames--;
			}
			if(get_bool(r, A_PYRAMID)) buttons |= BTN_PYRAMID;
			if(get_bool(r, A_BOX)) buttons |= BTN_BOX;
			if(get_bool(r, A_L3)) buttons |= BTN_L3;
			if(get_bool(r, A_R3)) buttons |= BTN_R3;
			if(get_float(r, A_L1) > 0.5f) buttons |= BTN_L1;
			if(get_float(r, A_R1) > 0.5f) buttons |= BTN_R1;
			l2 = (jint)(get_float(r, A_L2) * 255.0f);
			r2 = (jint)(get_float(r, A_R2) * 255.0f);
			// OpenXR y is up, DualSense y is down
			lx = stick_axis(ls.x); ly = stick_axis(-ls.y);
			rx = stick_axis(rs.x); ry = stick_axis(-rs.y);
		}
	}

	(*env)->CallVoidMethod(env, r->callback, r->on_input, buttons, l2, r2, lx, ly, rx, ry);
}

static void handle_state(XrRenderer *r, XrSessionState state)
{
	r->state = state;
	LOGI("Session state %d", (int)state);
	switch(state)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
			begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if(XR_SUCCEEDED(xrBeginSession(r->session, &begin)))
				r->running = true;
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			xrEndSession(r->session);
			r->running = false;
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			r->quit = true;
			break;
		default:
			break;
	}
}

static void *xr_thread(void *user)
{
	XrRenderer *r = user;
	JNIEnv *env = NULL;
	(*r->vm)->AttachCurrentThread(r->vm, &env, NULL);
	bool exit_requested_by_runtime = false;

	if(!egl_init(r))
	{
		LOGE("EGL init failed");
		goto fail;
	}

	PFN_xrInitializeLoaderKHR init_loader = NULL;
	XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader));
	XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loader_info.applicationVM = r->vm;
	loader_info.applicationContext = r->activity;
	XR_CHECK(init_loader((XrLoaderInitInfoBaseHeaderKHR *)&loader_info));

	const char *exts[] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
	};
	XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	android_info.applicationVM = r->vm;
	android_info.applicationActivity = r->activity;
	XrInstanceCreateInfo inst_info = { XR_TYPE_INSTANCE_CREATE_INFO };
	inst_info.next = &android_info;
	strcpy(inst_info.applicationInfo.applicationName, "Chiaki Immersive");
	inst_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	inst_info.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
	inst_info.enabledExtensionNames = exts;
	XR_CHECK(xrCreateInstance(&inst_info, &r->instance));

	XrSystemGetInfo sys_info = { XR_TYPE_SYSTEM_GET_INFO };
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(r->instance, &sys_info, &r->system));

	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_gles_req = NULL;
	XR_CHECK(xrGetInstanceProcAddr(r->instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&get_gles_req));
	XrGraphicsRequirementsOpenGLESKHR gles_req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XR_CHECK(get_gles_req(r->instance, r->system, &gles_req));

	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = r->egl_display;
	binding.config = r->egl_config;
	binding.context = r->egl_context;
	XrSessionCreateInfo sess_info = { XR_TYPE_SESSION_CREATE_INFO };
	sess_info.next = &binding;
	sess_info.systemId = r->system;
	XR_CHECK(xrCreateSession(r->instance, &sess_info, &r->session));

	XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	XR_CHECK(xrCreateReferenceSpace(r->session, &space_info, &r->space));

	if(!input_init(r))
		LOGE("Controller input setup failed, only physical gamepads will work");

	PFN_xrCreateSwapchainAndroidSurfaceKHR create_surface_swapchain = NULL;
	XR_CHECK(xrGetInstanceProcAddr(r->instance, "xrCreateSwapchainAndroidSurfaceKHR", (PFN_xrVoidFunction *)&create_surface_swapchain));
	XrSwapchainCreateInfo sc_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sc_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	sc_info.width = (uint32_t)r->width;
	sc_info.height = (uint32_t)r->height;
	sc_info.sampleCount = 1;
	sc_info.faceCount = 1;
	sc_info.arraySize = 1;
	sc_info.mipCount = 1;
	jobject surface = NULL;
	XR_CHECK(create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface));
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, surface);
	(*env)->DeleteLocalRef(env, surface);
	LOGI("Surface swapchain %dx%d handed to the decoder", r->width, r->height);

	while(!r->quit)
	{
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while(xrPollEvent(r->instance, &ev) == XR_SUCCESS)
		{
			if(ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
				if(s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING)
					exit_requested_by_runtime = true;
				handle_state(r, s);
			}
			else if(ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
			{
				exit_requested_by_runtime = true;
				r->quit = true;
			}
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		}

		if(!r->running)
		{
			poll_input(r, env); // reports a neutral state while unfocused
			usleep(20000);
			continue;
		}

		XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
		if(XR_FAILED(xrWaitFrame(r->session, &wait_info, &frame_state)))
			continue;
		XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(r->session, &begin_info);

		poll_input(r, env);

		XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		quad.space = r->space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = r->swapchain;
		quad.subImage.imageRect.extent.width = r->width;
		quad.subImage.imageRect.extent.height = r->height;
		quad.pose.orientation.w = 1.0f;
		quad.pose.position.z = -SCREEN_DISTANCE_M;
		quad.size.width = SCREEN_WIDTH_M;
		quad.size.height = SCREEN_WIDTH_M * (float)r->height / (float)r->width;
		const XrCompositionLayerBaseHeader *layers[] = { (XrCompositionLayerBaseHeader *)&quad };

		XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
		end_info.displayTime = frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end_info.layerCount = frame_state.shouldRender ? 1 : 0;
		end_info.layers = layers;
		xrEndFrame(r->session, &end_info);
	}

fail:
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, NULL);
	if(r->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->swapchain);
	if(r->action_set != XR_NULL_HANDLE) xrDestroyActionSet(r->action_set);
	if(r->space != XR_NULL_HANDLE) xrDestroySpace(r->space);
	if(r->session != XR_NULL_HANDLE) xrDestroySession(r->session);
	if(r->instance != XR_NULL_HANDLE) xrDestroyInstance(r->instance);
	egl_fini(r);
	// Tell the activity to finish unless it asked us to stop in the first place
	if(exit_requested_by_runtime || !r->quit)
		(*env)->CallVoidMethod(env, r->callback, r->on_exit);
	(*r->vm)->DetachCurrentThread(r->vm);
	return NULL;
}

JNIEXPORT jlong JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeStart(JNIEnv *env, jobject obj, jobject activity, jint width, jint height)
{
	XrRenderer *r = calloc(1, sizeof(XrRenderer));
	if(!r)
		return 0;
	(*env)->GetJavaVM(env, &r->vm);
	r->activity = (*env)->NewGlobalRef(env, activity);
	r->callback = (*env)->NewGlobalRef(env, obj);
	jclass cls = (*env)->GetObjectClass(env, obj);
	r->on_surface = (*env)->GetMethodID(env, cls, "onSurface", "(Landroid/view/Surface;)V");
	r->on_input = (*env)->GetMethodID(env, cls, "onInput", "(IIISSSS)V");
	r->on_exit = (*env)->GetMethodID(env, cls, "onExit", "()V");
	r->width = width;
	r->height = height;
	r->egl_display = EGL_NO_DISPLAY;
	r->egl_context = EGL_NO_CONTEXT;
	r->egl_surface = EGL_NO_SURFACE;
	if(pthread_create(&r->thread, NULL, xr_thread, r) != 0)
	{
		(*env)->DeleteGlobalRef(env, r->activity);
		(*env)->DeleteGlobalRef(env, r->callback);
		free(r);
		return 0;
	}
	return (jlong)r;
}

JNIEXPORT void JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeStop(JNIEnv *env, jobject obj, jlong ptr)
{
	XrRenderer *r = (XrRenderer *)ptr;
	if(!r)
		return;
	r->quit = true;
	pthread_join(r->thread, NULL);
	(*env)->DeleteGlobalRef(env, r->activity);
	(*env)->DeleteGlobalRef(env, r->callback);
	free(r);
}
