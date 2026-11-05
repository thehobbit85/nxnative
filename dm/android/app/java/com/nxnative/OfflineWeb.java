package com.nxnative;

import android.graphics.Color;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.ColorDrawable;
import android.util.AtomicFile;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.webkit.JavascriptInterface;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import org.json.JSONObject;

/** One offline applet, owned entirely by the Activity's UI thread. */
final class OfflineWeb extends FrameLayout {
    final long id;
    private final GameActivity activity;
    private final WebView view;
    private final PopupWindow window;
    private final String callback;
    private final ArrayList<String> pending = new ArrayList<>();
    private boolean shown, closed, loaded, paused, destroyed;
    private long mask;
    private String lastPad = "";
    OfflineWeb(GameActivity activity, long id, String dir, String path, String callback, String font,
               boolean hidden, int delay, int pulse, boolean muted) throws Exception {
        super(activity); this.activity = activity; this.id = id; this.callback = callback;
        String shim;
        try (InputStream in = activity.getAssets().open("web-shim.js")) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            shim = out.toString("UTF-8");
        }
        String script = "window.__dm_cfg={muted:" + muted + ",repeat_delay:" + delay + ",repeat_pulse:" + pulse + "};"
            + "window.webkit={messageHandlers:{dm:{postMessage:function(m){DmWeb.postMessage(JSON.stringify(m));}}}};" + shim;
        OfflineWebResources resources = new OfflineWebResources(new File(dir), path, font.isEmpty() ? null : new File(font), script);
        view = new WebView(activity);
        view.setBackgroundColor(Color.TRANSPARENT);
        view.getSettings().setJavaScriptEnabled(true);
        view.getSettings().setAllowFileAccess(false); view.getSettings().setAllowContentAccess(false);
        view.getSettings().setBlockNetworkLoads(true); view.getSettings().setCacheMode(WebSettings.LOAD_NO_CACHE);
        view.getSettings().setMediaPlaybackRequiresUserGesture(false);
        view.setHorizontalScrollBarEnabled(false); view.setVerticalScrollBarEnabled(false);
        view.addJavascriptInterface(new Bridge(), "DmWeb");
        view.setWebViewClient(new WebViewClient() {
            @Override public WebResourceResponse shouldInterceptRequest(WebView web, WebResourceRequest request) {
                OfflineWebResources.Resource resource = resources.open(request.getUrl().toString());
                return new WebResourceResponse(resource.mime, "UTF-8", resource.status, resource.status == 200 ? "OK" : "Not Found",
                    Collections.singletonMap("Cache-Control", "no-store"), resource.body);
            }
            @Override public boolean shouldOverrideUrlLoading(WebView web, WebResourceRequest request) {
                String url = request.getUrl().toString();
                if (!callback.isEmpty() && url.startsWith(callback)) { end(3, url); return true; }
                String scheme = request.getUrl().getScheme();
                if ("about".equals(scheme) || "data".equals(scheme) || "blob".equals(scheme)) return false;
                try { return !OfflineWebResources.local(new java.net.URI(url)); }
                catch (java.net.URISyntaxException error) { return true; }
            }
            @Override public void onPageFinished(WebView web, String url) {
                if (closed) return;
                loaded = true;
                for (String message : pending) deliver(message);
                pending.clear();
            }
            @Override public void onReceivedError(WebView web, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) failed(error.getDescription().toString());
            }
            @Override public void onReceivedHttpError(WebView web, WebResourceRequest request, WebResourceResponse response) {
                if (request.isForMainFrame()) failed("HTTP " + response.getStatusCode() + " loading offline document");
            }
            @Override public boolean onRenderProcessGone(WebView web, RenderProcessGoneDetail detail) { end(4, ""); close(); return true; }
        });
        addView(view); setClipChildren(false); setClipToPadding(false); setAlpha(0);
        // NativeActivity takes ownership of its entire window surface for
        // Vulkan. An ordinary child view has no UI surface to draw into there.
        // This application panel gets a separate transparent composited surface.
        window = new PopupWindow(this, -1, -1, false) {
            @Override public void dismiss() {
                // PopupDecorView consumes Back/ESC (and predictive Back) before
                // the content. Keep the panel until the page handles B, which
                // may navigate history or invoke a custom footer callback.
                if (active() && !paused) back(); else super.dismiss();
            }
        };
        window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        window.setTouchable(false); window.setClippingEnabled(false); window.setIsLaidOutInScreen(true);
        window.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        attach();
        view.loadUrl(OfflineWebResources.ORIGIN + path);
        if (!hidden) show();
    }
    private final class Bridge {
        @JavascriptInterface public void postMessage(String message) {
            activity.runOnUiThread(() -> {
                if (closed) return;
                try {
                    JSONObject value = new JSONObject(message); String kind = value.optString("k");
                    if (kind.equals("msg")) GameActivity.nativeWebEvent(id, 0, 0, value.optString("d").getBytes(StandardCharsets.UTF_8));
                    else if (kind.equals("end")) end(value.optInt("r"), "");
                    else if (kind.equals("log")) GameActivity.nativeWebEvent(id, 3, 0, value.optString("d").getBytes(StandardCharsets.UTF_8));
                } catch (org.json.JSONException error) { android.util.Log.w("nxnative", "Invalid offline page message", error); }
            });
        }
    }
    @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        float density = getResources().getDisplayMetrics().density;
        int width = Math.round(1280 * density), height = Math.round(720 * density);
        float scale = Math.min((right - left) / (float)width, (bottom - top) / (float)height);
        int x = Math.round(((right - left) - width * scale) / 2), y = Math.round(((bottom - top) - height * scale) / 2);
        view.layout(x, y, x + width, y + height); view.setPivotX(0); view.setPivotY(0); view.setScaleX(scale); view.setScaleY(scale);
    }
    @Override public boolean dispatchTouchEvent(MotionEvent event) { return shown && !closed && super.dispatchTouchEvent(event); }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        return active() && activity.dispatchKeyEvent(event) || super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (active() && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return activity.dispatchGenericMotionEvent(event);
        return super.dispatchGenericMotionEvent(event);
    }
    private void attach() {
        setSystemUiVisibility(activity.getWindow().getDecorView().getSystemUiVisibility());
        if (!window.isShowing()) window.showAtLocation(activity.getWindow().getDecorView(), Gravity.TOP | Gravity.LEFT, 0, 0);
    }
    boolean active() { return shown && !closed; }
    private void eval(String js) { if (!closed) view.evaluateJavascript(js, null); }
    private void deliver(String message) { eval("window.__dm&&__dm.recv(" + JSONObject.quote(message) + ")"); }
    void send(String message) { if (!closed) { if (loaded) deliver(message); else pending.add(message); } }
    private void shotLog(String message) { GameActivity.nativeWebEvent(id, 3, 0, message.getBytes(StandardCharsets.UTF_8)); }
    void shot(String path) {
        if (closed || paused || !loaded) { shotLog("webshot: no ready page"); return; }
        // DOM messages can arrive before Chromium has a drawable frame. Draw
        // after this callback completes, on the owning UI thread.
        view.postVisualStateCallback(0, new WebView.VisualStateCallback() {
            @Override public void onComplete(long requestId) { view.post(() -> saveShot(path)); }
        });
    }
    private void saveShot(String path) {
        if (closed || paused || view.getWidth() <= 0 || view.getHeight() <= 0) { shotLog("webshot: no ready page"); return; }
        Bitmap bitmap = null; FileOutputStream stream = null;
        AtomicFile file = new AtomicFile(new File(path));
        try {
            bitmap = Bitmap.createBitmap(view.getWidth(), view.getHeight(), Bitmap.Config.ARGB_8888);
            view.draw(new Canvas(bitmap));
            stream = file.startWrite();
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)) throw new IOException("PNG encoding failed");
            file.finishWrite(stream); stream = null;
            shotLog("webshot: saved " + path);
        } catch (IOException | RuntimeException | OutOfMemoryError error) {
            file.failWrite(stream); shotLog("[RED] webshot: nothing written to " + path + " (" + error + ")");
        } finally { if (bitmap != null) bitmap.recycle(); }
    }
    void show() {
        if (closed || shown) return;
        shown = true; setAlpha(1); window.setFocusable(true); window.setTouchable(true); window.update(); view.requestFocus();
        long[] pad = GameActivity.nativeWebPad(); mask = pad[0]; lastPad = "";
        eval("window.__dm&&__dm.show()"); removeCallbacks(poll); post(poll);
    }
    void pause(boolean value) {
        paused = value; removeCallbacks(poll);
        if (value) { view.onPause(); window.dismiss(); }
        else if (!closed) {
            attach(); view.onResume();
            // Reattaching a popup reparents its content and clears descendant
            // focus. Without this, the focused window receives keys but none
            // reach dispatchKeyEvent on the content after HOME/resume.
            if (active()) { view.requestFocus(); post(poll); }
        }
    }
    void back() { eval("window.__dm&&(__dm.pad(2,0,0,0,0),__dm.pad(0,0,0,0,0))"); }
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (!active() || paused) return;
            long[] pad = GameActivity.nativeWebPad(); mask &= pad[0]; pad[0] &= ~mask;
            for (int i = 1; i < 5; i++) if (Math.abs(pad[i]) < 32767 / 8) pad[i] = 0;
            String args = pad[0] + "," + pad[1] + "," + pad[2] + "," + pad[3] + "," + pad[4];
            if (!args.equals(lastPad)) {
                lastPad = args; eval("window.__dm&&__dm.pad(" + args + ")");
            }
            postDelayed(this, 16);
        }
    };
    private void end(int reason, String url) {
        if (closed) return;
        closed = true; shown = false; setAlpha(0); removeCallbacks(poll);
        // Release the input window before reporting completion. The guest may
        // resume playing long before it collects this session's return value.
        window.dismiss();
        GameActivity.nativeWebEvent(id, 1, reason, url.getBytes(StandardCharsets.UTF_8));
    }
    private void failed(String why) {
        if (loaded || closed) return;
        GameActivity.nativeWebEvent(id, 2, 0, why.getBytes(StandardCharsets.UTF_8)); close();
    }
    void close() {
        if (destroyed) return; destroyed = true;
        closed = true; shown = false; removeCallbacks(poll); pending.clear();
        view.removeJavascriptInterface("DmWeb"); view.stopLoading();
        window.dismiss();
        removeView(view); view.destroy();
    }
}
