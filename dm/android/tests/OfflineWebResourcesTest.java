package com.nxnative;

import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

public final class OfflineWebResourcesTest {
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    static void write(File file, byte[] data) throws Exception { file.getParentFile().mkdirs(); Files.write(file.toPath(), data); }
    static byte[] read(OfflineWebResources.Resource r) throws Exception { try (InputStream in = r.body) { return in.readAllBytes(); } }
    public static void main(String[] args) throws Exception {
        File tmp = Files.createTempDirectory("nx-web").toFile(), root = new File(tmp, "doc");
        byte[] image = new byte[]{0, 1, (byte)255, 3}, font = new byte[]{4, 5, 6};
        try {
            write(new File(root, "menu/index.html"), "\ufeff<!DOCTYPE html>\n<html><script>window.page=1;</script></html>".getBytes(StandardCharsets.UTF_8));
            write(new File(root, "menu/img/icon.png"), image);
            write(new File(root, "menu/space name.css"), "body{}".getBytes(StandardCharsets.UTF_8));
            write(new File(tmp, "outside.txt"), "private".getBytes(StandardCharsets.UTF_8));
            File ext = new File(tmp, "font.ttf"); write(ext, font);
            Files.createSymbolicLink(new File(root, "escape").toPath(), new File(tmp, "outside.txt").toPath());
            OfflineWebResources res = new OfflineWebResources(root, "menu/index.html?lang=en", ext, "window.beforePage=true;");
            OfflineWebResources.Resource html = res.open(OfflineWebResources.ORIGIN + "menu/index.html?lang=en#start");
            check(html.status == 200 && html.mime.equals("text/html"), "page response");
            String text = new String(read(html), StandardCharsets.UTF_8);
            check(text.indexOf("<!DOCTYPE html>") < text.indexOf("window.beforePage"), "lost standards mode");
            check(text.indexOf("window.beforePage") < text.indexOf("window.page"), "shim after inline game script");
            check(Arrays.equals(read(res.open(OfflineWebResources.ORIGIN + "menu/img/icon.png")), image), "binary resource changed");
            check(Arrays.equals(read(res.open(OfflineWebResources.ORIGIN + "img/icon.png")), image), "document-folder fallback");
            OfflineWebResources.Resource css = res.open(OfflineWebResources.ORIGIN + "menu/space%20name.css");
            check(css.mime.equals("text/css") && read(css).length == 6, "encoded path or MIME");
            check(Arrays.equals(read(res.open(OfflineWebResources.ORIGIN + "__dm_font/nintendo_ext.ttf")), font), "explicit extension font");
            for (String url : new String[]{"https://example.invalid/file", "file:///etc/passwd", "https://nxnative.invalid.evil/file",
                    "https://user@nxnative.invalid/file", "https://nxnative.invalid:123/file", "invalid URL", "data:text/html,hello",
                    OfflineWebResources.ORIGIN + "escape", OfflineWebResources.ORIGIN + "../outside.txt",
                    OfflineWebResources.ORIGIN + "%2e%2e/outside.txt", OfflineWebResources.ORIGIN + "missing"}) {
                OfflineWebResources.Resource missing = res.open(url);
                check(missing.status == 404 && read(missing).length == 0, "served outside document: " + url);
            }
            write(new File(root, "plain.htm"), "<html><script>page()</script></html>".getBytes(StandardCharsets.UTF_8));
            check(new String(read(res.open(OfflineWebResources.ORIGIN + "plain.htm")), StandardCharsets.UTF_8).startsWith("<script>window.beforePage"), "non-doctype page");
            write(new File(root, "nested/index.html"), "<!doctype html><p>nested</p>".getBytes(StandardCharsets.UTF_8));
            check(new String(read(res.open(OfflineWebResources.ORIGIN + "nested/index.html")), StandardCharsets.UTF_8).contains("window.beforePage"), "shim omitted from another page");
        } finally { AssetInstall.remove(tmp); }
        System.out.println("Offline HTML resources: document isolation, binary data, fonts, fallback, MIME and early script injection passed");
    }
}
