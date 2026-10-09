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
#include <math.h>
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
#define BTN_MENU_OPEN (1 << 30) // not a DualSense button, stripped in XrRenderer.kt

#define DPAD_THRESHOLD 0.5f
#define OPTIONS_TAP_FRAMES 8 // ~90 ms at 90 Hz, long enough for the console to see the press

#define SCREEN_WIDTH_M 3.2f
#define SCREEN_DISTANCE_M 2.4f

// Secret menu: Menu + L1 + L2 + R1 + R2. Images come from Kotlin (XrRenderer.menuImage)
#define MENU_IMG_SIZE 512
#define MENU_IMG_COUNT 7 // 0 idle, 1 left, 2 up, 3 right, 4 down, 5 resize hint, 6 move hint
#define MENU_SIZE_M 0.6f
#define MENU_DISTANCE_M 1.0f
#define MENU_DIR_THRESHOLD 0.6f

enum { MODE_NONE, MODE_MENU, MODE_RESIZE, MODE_MOVE };

enum {
	A_CROSS, A_MOON, A_BOX, A_PYRAMID, A_L3, A_R3, A_MENU,
	A_L2, A_R2, A_L1, A_R1, A_LSTICK, A_RSTICK, A_COUNT
};

typedef struct XrRenderer
{
	JavaVM *vm;
	jobject activity; // global ref
	jobject callback; // global ref, the Kotlin XrRenderer
	jmethodID on_surface, on_input, on_exit, menu_image;
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

	// Screen placement, editable from the secret menu
	XrPosef screen_pose;
	float screen_width;
	float screen_distance;
	bool flip_ext; // XR_FB_composition_layer_image_layout available

	// Secret menu
	XrSpace view_space;
	XrSwapchain menu_swapchain;
	GLuint menu_textures[MENU_IMG_COUNT];
	GLuint menu_fbo[2];
	int menu_shown_img; // image currently in the swapchain, -1 = none yet
	int mode;
	int menu_dir; // 0 none, 1..4 like the image indices
	XrPosef menu_pose;
	bool prev_confirm, prev_combo;
	bool release_guard; // menu just closed: keep the pad neutral until everything is let go
	bool user_exit;
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

static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b)
{
	XrQuaternionf q = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
	return q;
}

static XrTime frame_time; // predicted display time of the frame being built

// Pose `distance` metres in front of the head, kept upright (yaw only)
static bool pose_in_front_of_head(XrRenderer *r, float distance, bool follow_pitch, XrPosef *out)
{
	if(r->view_space == XR_NULL_HANDLE || !frame_time)
		return false;
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if(XR_FAILED(xrLocateSpace(r->view_space, r->space, frame_time, &loc))
			|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			|| !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return false;
	XrQuaternionf q = loc.pose.orientation;
	// forward = q * (0,0,-1), flattened onto the floor plane
	float fx = -2.0f * (q.x * q.z + q.w * q.y);
	float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	float len = sqrtf(fx * fx + fz * fz);
	if(len < 1e-4f)
		return false;
	fx /= len; fz /= len;
	float yaw = atan2f(-fx, -fz);
	float pitch = 0.0f;
	if(follow_pitch)
	{
		float fy = 2.0f * (q.w * q.x - q.y * q.z);
		if(fy > 1.0f) fy = 1.0f;
		if(fy < -1.0f) fy = -1.0f;
		pitch = asinf(fy);
		// small nods keep the screen upright, only a clear look up/down tilts it
		const float dead = 8.0f * 3.14159265f / 180.0f;
		if(fabsf(pitch) < dead)
			pitch = 0.0f;
	}
	XrQuaternionf qyaw = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
	XrQuaternionf qpitch = { sinf(pitch * 0.5f), 0.0f, 0.0f, cosf(pitch * 0.5f) };
	// the quad faces +Z, so tilting it by the head pitch keeps it facing the viewer
	out->orientation = quat_mul(qyaw, qpitch);
	float cp = cosf(pitch);
	out->position.x = loc.pose.position.x + fx * cp * distance;
	out->position.y = loc.pose.position.y + sinf(pitch) * distance;
	out->position.z = loc.pose.position.z + fz * cp * distance;
	return true;
}

static int stick_dir(XrVector2f v)
{
	if(fabsf(v.x) < MENU_DIR_THRESHOLD && fabsf(v.y) < MENU_DIR_THRESHOLD)
		return 0;
	if(fabsf(v.x) > fabsf(v.y))
		return v.x < 0 ? 1 : 3;
	return v.y > 0 ? 2 : 4;
}

static float stronger_y(XrVector2f a, XrVector2f b)
{
	return fabsf(a.y) > fabsf(b.y) ? a.y : b.y;
}

// Returns true while the secret menu (or one of its modes) owns the controllers
static bool handle_menu(XrRenderer *r)
{
	bool combo = get_bool(r, A_MENU) && get_float(r, A_L1) > 0.5f && get_float(r, A_R1) > 0.5f
		&& get_float(r, A_L2) > 0.5f && get_float(r, A_R2) > 0.5f;
	bool confirm = get_bool(r, A_CROSS);
	bool confirm_edge = confirm && !r->prev_confirm;
	bool combo_edge = combo && !r->prev_combo;
	r->prev_confirm = confirm;
	r->prev_combo = combo;

	if(r->mode == MODE_NONE && r->release_guard)
	{
		XrVector2f gl = get_vec2(r, A_LSTICK), gr = get_vec2(r, A_RSTICK);
		bool any = get_bool(r, A_MENU) || confirm || get_bool(r, A_MOON) || get_bool(r, A_BOX)
			|| get_bool(r, A_PYRAMID) || get_bool(r, A_L3) || get_bool(r, A_R3)
			|| get_float(r, A_L1) > 0.2f || get_float(r, A_R1) > 0.2f
			|| get_float(r, A_L2) > 0.2f || get_float(r, A_R2) > 0.2f
			|| fabsf(gl.x) > 0.3f || fabsf(gl.y) > 0.3f || fabsf(gr.x) > 0.3f || fabsf(gr.y) > 0.3f;
		if(any)
			return true;
		r->release_guard = false;
		r->menu_held = false;
		return false;
	}

	if(r->mode == MODE_NONE)
	{
		if(!combo_edge || r->menu_swapchain == XR_NULL_HANDLE)
			return false;
		if(!pose_in_front_of_head(r, MENU_DISTANCE_M, false, &r->menu_pose))
		{
			r->menu_pose.orientation = (XrQuaternionf){ 0, 0, 0, 1 };
			r->menu_pose.position = (XrVector3f){ 0, 0, -MENU_DISTANCE_M };
		}
		r->mode = MODE_MENU;
		r->menu_dir = 0;
		r->menu_combo = true; // releasing Menu must not send Options
		LOGI("Secret menu opened");
		return true;
	}

	XrVector2f ls = get_vec2(r, A_LSTICK);
	XrVector2f rs = get_vec2(r, A_RSTICK);
	switch(r->mode)
	{
		case MODE_MENU:
		{
			int d = stick_dir(rs);
			if(!d)
				d = stick_dir(ls);
			if(d)
				r->menu_dir = d;
			if(confirm_edge && r->menu_dir)
			{
				switch(r->menu_dir)
				{
					case 1: r->mode = MODE_RESIZE; break;
					case 2: r->mode = MODE_NONE; break;
					case 3: r->mode = MODE_MOVE; break;
					case 4: r->mode = MODE_NONE; r->user_exit = true; r->quit = true; break;
				}
				r->menu_dir = 0;
			}
			break;
		}
		case MODE_RESIZE:
		{
			float y = stronger_y(rs, ls);
			if(fabsf(y) > 0.15f)
				r->screen_width *= 1.0f + y * 0.015f;
			if(r->screen_width < 0.8f) r->screen_width = 0.8f;
			if(r->screen_width > 12.0f) r->screen_width = 12.0f;
			if(confirm_edge)
				r->mode = MODE_NONE;
			break;
		}
		case MODE_MOVE:
		{
			float y = stronger_y(rs, ls);
			if(fabsf(y) > 0.15f)
				r->screen_distance *= 1.0f - y * 0.015f;
			if(r->screen_distance < 0.8f) r->screen_distance = 0.8f;
			if(r->screen_distance > 10.0f) r->screen_distance = 10.0f;
			pose_in_front_of_head(r, r->screen_distance, true, &r->screen_pose);
			if(confirm_edge)
				r->mode = MODE_NONE;
			break;
		}
	}
	if(r->mode == MODE_NONE)
		r->release_guard = true;
	return true;
}

// Loads the menu images from Kotlin into GL textures and creates the menu swapchain
static bool menu_init(XrRenderer *r, JNIEnv *env)
{
	XrReferenceSpaceCreateInfo view_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	view_info.poseInReferenceSpace.orientation.w = 1.0f;
	XrResult res = xrCreateReferenceSpace(r->session, &view_info, &r->view_space);
	if(XR_FAILED(res)) { LOGE("menu: view space %d", res); return false; }

	// Quest supports these; try them in order instead of trusting the format enumeration
	static const int64_t formats[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.width = info.height = MENU_IMG_SIZE;
	info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
	res = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	for(int i = 0; i < 2 && XR_FAILED(res); i++)
	{
		info.format = formats[i];
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		res = xrCreateSwapchain(r->session, &info, &r->menu_swapchain);
		if(XR_FAILED(res))
		{
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			res = xrCreateSwapchain(r->session, &info, &r->menu_swapchain);
		}
		LOGI("menu: swapchain format 0x%x: %d", (int)info.format, res);
	}
	if(XR_FAILED(res))
		return false;

	glGenTextures(MENU_IMG_COUNT, r->menu_textures);
	glGenFramebuffers(2, r->menu_fbo);
	for(int i = 0; i < MENU_IMG_COUNT; i++)
	{
		jbyteArray pixels = (*env)->CallObjectMethod(env, r->callback, r->menu_image, i, MENU_IMG_SIZE);
		if((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
		if(!pixels) { LOGE("menu: image %d missing", i); return false; }
		jbyte *data = (*env)->GetByteArrayElements(env, pixels, NULL);
		glBindTexture(GL_TEXTURE_2D, r->menu_textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		(*env)->ReleaseByteArrayElements(env, pixels, data, JNI_ABORT);
		(*env)->DeleteLocalRef(env, pixels);
	}
	r->menu_shown_img = -1;
	return true;
}

static void menu_draw(XrRenderer *r, int img)
{
	if(img == r->menu_shown_img)
		return; // the last released image is still valid
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(XR_FAILED(xrAcquireSwapchainImage(r->menu_swapchain, &acq, &index)))
		return;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(r->menu_swapchain, &wait);

	XrSwapchainImageOpenGLESKHR images[8];
	uint32_t n = 0;
	for(int i = 0; i < 8; i++)
		images[i] = (XrSwapchainImageOpenGLESKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
	xrEnumerateSwapchainImages(r->menu_swapchain, 8, &n, (XrSwapchainImageBaseHeader *)images);
	if(index < n)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, r->menu_fbo[0]);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->menu_textures[img], 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->menu_fbo[1]);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
		// Bitmaps are top row first, GL images bottom row first
		glBlitFramebuffer(0, 0, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glFinish();
	}
	XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(r->menu_swapchain, &rel);
	r->menu_shown_img = img;
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
		if(xrSyncActions(r->session, &sync) == XR_SUCCESS && !handle_menu(r))
		{
			bool menu = get_bool(r, A_MENU);
			static int dbg;
			if(++dbg % 90 == 0)
			{
				XrVector2f l = get_vec2(r, A_LSTICK), rr = get_vec2(r, A_RSTICK);
				LOGI("in: menu=%d A=%d X=%d L1=%.2f R1=%.2f L2=%.2f R2=%.2f ls=%.2f,%.2f rs=%.2f,%.2f",
					menu, get_bool(r, A_CROSS), get_bool(r, A_BOX), get_float(r, A_L1), get_float(r, A_R1),
					get_float(r, A_L2), get_float(r, A_R2), l.x, l.y, rr.x, rr.y);
			}
			bool cross = get_bool(r, A_CROSS);
			bool moon = get_bool(r, A_MOON);
			XrVector2f ls = get_vec2(r, A_LSTICK);
			XrVector2f rs = get_vec2(r, A_RSTICK);

			// Menu is a modifier: Menu + A = PS, Menu + B = Share, Menu + right stick = D-pad.
			// Options is only sent as a tap when Menu is released without any combo.
			if(menu)
			{
				if(!r->menu_held)
					r->menu_combo = false;
				if(cross) { buttons |= BTN_PS; r->menu_combo = true; }
				if(moon) { buttons |= BTN_SHARE; r->menu_combo = true; }
				if(rs.y > DPAD_THRESHOLD) { buttons |= BTN_DPAD_UP; r->menu_combo = true; }
				if(rs.y < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_DOWN; r->menu_combo = true; }
				if(rs.x < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_LEFT; r->menu_combo = true; }
				if(rs.x > DPAD_THRESHOLD) { buttons |= BTN_DPAD_RIGHT; r->menu_combo = true; }
				rs.x = rs.y = 0.0f; // the stick is the D-pad while Menu is held
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
			// While Menu is held the shoulders and triggers belong to the secret menu combo,
			// so the console never sees them
			if(!menu)
			{
				if(get_float(r, A_L1) > 0.5f) buttons |= BTN_L1;
				if(get_float(r, A_R1) > 0.5f) buttons |= BTN_R1;
				l2 = (jint)(get_float(r, A_L2) * 255.0f);
				r2 = (jint)(get_float(r, A_R2) * 255.0f);
			}
			else if(get_float(r, A_L1) > 0.5f || get_float(r, A_R1) > 0.5f
					|| get_float(r, A_L2) > 0.5f || get_float(r, A_R2) > 0.5f)
				r->menu_combo = true; // no Options tap on release either
			// OpenXR y is up, DualSense y is down
			lx = stick_axis(ls.x); ly = stick_axis(-ls.y);
			rx = stick_axis(rs.x); ry = stick_axis(-rs.y);
		}
	}

	if(r->mode != MODE_NONE || r->release_guard)
		buttons = BTN_MENU_OPEN; // neutral pad, and tells Kotlin to ignore Android gamepad events
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

	// The decoder writes the surface top row first, which the compositor shows upside down;
	// XR_FB_composition_layer_image_layout flips it, otherwise the quad is turned around instead
	uint32_t ext_count = 0;
	xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL);
	XrExtensionProperties *ext_props = calloc(ext_count ? ext_count : 1, sizeof(XrExtensionProperties));
	for(uint32_t i = 0; i < ext_count; i++)
		ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, ext_props);
	for(uint32_t i = 0; i < ext_count; i++)
		if(!strcmp(ext_props[i].extensionName, XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME))
			r->flip_ext = true;
	free(ext_props);
	LOGI("Image layout flip extension: %d", r->flip_ext);
	const char *exts[] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
		XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME,
	};
	XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	android_info.applicationVM = r->vm;
	android_info.applicationActivity = r->activity;
	XrInstanceCreateInfo inst_info = { XR_TYPE_INSTANCE_CREATE_INFO };
	inst_info.next = &android_info;
	strcpy(inst_info.applicationInfo.applicationName, "Chiaki Immersive");
	inst_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	inst_info.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]) - (r->flip_ext ? 0 : 1);
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
	if(!menu_init(r, env))
		LOGE("Secret menu setup failed");

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
	// Runtimes disagree on what a surface swapchain may carry; try the known variants in turn
	XrResult sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
	LOGI("surface swapchain (sampled|color, counts 1): %d", sc_res);
	if(XR_FAILED(sc_res))
	{
		sc_info.usageFlags = 0;
		sc_info.sampleCount = sc_info.faceCount = sc_info.arraySize = sc_info.mipCount = 0;
		sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
		LOGI("surface swapchain (no usage, counts 0): %d", sc_res);
	}
	if(XR_FAILED(sc_res))
	{
		sc_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		sc_info.format = 0x8058; // GL_RGBA8
		sc_info.sampleCount = sc_info.faceCount = sc_info.arraySize = sc_info.mipCount = 1;
		sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
		LOGI("surface swapchain (RGBA8, sampled): %d", sc_res);
	}
	XR_CHECK(sc_res);
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, surface);
	// The runtime owns this reference (Meta hands out a global ref); don't delete it
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

		frame_time = frame_state.predictedDisplayTime;
		poll_input(r, env);

		XrCompositionLayerImageLayoutFB layout = { XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB };
		layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
		XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		quad.space = r->space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = r->swapchain;
		quad.subImage.imageRect.extent.width = r->width;
		quad.subImage.imageRect.extent.height = r->height;
		quad.pose = r->screen_pose;
		if(r->flip_ext)
			quad.next = &layout;
		else
		{
			// Seen from behind after half a turn around X: a pure vertical flip
			XrQuaternionf flip = { 1.0f, 0.0f, 0.0f, 0.0f };
			quad.pose.orientation = quat_mul(quad.pose.orientation, flip);
		}
		quad.size.width = r->screen_width;
		quad.size.height = r->screen_width * (float)r->height / (float)r->width;
		const XrCompositionLayerBaseHeader *layers[2] = { (XrCompositionLayerBaseHeader *)&quad };
		uint32_t layer_count = 1;

		XrCompositionLayerQuad menu = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		if(r->mode != MODE_NONE && frame_state.shouldRender)
		{
			int img = r->mode == MODE_RESIZE ? 5 : r->mode == MODE_MOVE ? 6 : r->menu_dir;
			menu_draw(r, img);
			menu.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			menu.space = r->space;
			menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			menu.subImage.swapchain = r->menu_swapchain;
			menu.subImage.imageRect.extent.width = MENU_IMG_SIZE;
			menu.subImage.imageRect.extent.height = MENU_IMG_SIZE;
			menu.pose = r->menu_pose;
			menu.size.width = menu.size.height = MENU_SIZE_M;
			layers[layer_count++] = (XrCompositionLayerBaseHeader *)&menu;
		}

		XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
		end_info.displayTime = frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end_info.layerCount = frame_state.shouldRender ? layer_count : 0;
		end_info.layers = layers;
		xrEndFrame(r->session, &end_info);
	}

fail:
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, NULL);
	if(r->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->swapchain);
	if(r->menu_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->menu_swapchain);
	if(r->view_space != XR_NULL_HANDLE) xrDestroySpace(r->view_space);
	if(r->action_set != XR_NULL_HANDLE) xrDestroyActionSet(r->action_set);
	if(r->space != XR_NULL_HANDLE) xrDestroySpace(r->space);
	if(r->session != XR_NULL_HANDLE) xrDestroySession(r->session);
	if(r->instance != XR_NULL_HANDLE) xrDestroyInstance(r->instance);
	egl_fini(r);
	// Tell the activity to finish unless it asked us to stop in the first place
	if(exit_requested_by_runtime || !r->quit || r->user_exit)
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
	r->menu_image = (*env)->GetMethodID(env, cls, "menuImage", "(II)[B");
	r->width = width;
	r->height = height;
	r->screen_pose.orientation.w = 1.0f;
	r->screen_pose.position.z = -SCREEN_DISTANCE_M;
	r->screen_width = SCREEN_WIDTH_M;
	r->screen_distance = SCREEN_DISTANCE_M;
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
