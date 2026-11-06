package com.nxnative;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import java.io.File;
import java.io.IOException;

/** Publish ADB-staged game assets as the UID that will open and recover them. */
public final class AssetDeployProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }

    @Override public Bundle call(String method, String expectedApk, Bundle extras) {
        int uid = Binder.getCallingUid();
        if (uid != 2000 && uid != 0) throw new SecurityException("ADB shell required");
        // ContentProvider.call has no per-method permission checks. Only the
        // shell can request this fixed transaction; paths come from the app.
        Context app = getContext();
        File apk = new File(app.getApplicationInfo().sourceDir);
        if (!apk.getPath().equals(expectedApk)) throw new IllegalStateException("Installed APK changed");
        long caller = Binder.clearCallingIdentity();
        try {
            GameAssets.Manifest manifest = AssetDeploy.readManifest(apk);
            if ("install".equals(method)) {
                File root = app.getExternalFilesDir(null);
                if (root == null) throw new IOException("Primary app storage is unavailable");
                AssetDeploy.install(root, manifest, AssetInstall::move);
            } else if (!"check".equals(method)) {
                throw new IllegalArgumentException("Unknown game asset operation");
            }
            Bundle result = new Bundle();
            result.putString("identity", manifest.identity);
            return result;
        } catch (IOException error) {
            throw new IllegalStateException(error.getMessage(), error);
        } finally {
            Binder.restoreCallingIdentity(caller);
        }
    }
    @Override public String getType(Uri uri) { return null; }
    @Override public Cursor query(Uri uri, String[] columns, String where, String[] args, String sort) { throw new UnsupportedOperationException(); }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String where, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String where, String[] args) { throw new UnsupportedOperationException(); }
}
