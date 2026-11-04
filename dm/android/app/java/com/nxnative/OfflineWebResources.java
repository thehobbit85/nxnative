package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Offline documents use an HTTPS origin so their relative XHR/fetch works.
 * Every request is answered locally, including failures; never fall through to
 * the network or let a document reach another program's storage. */
final class OfflineWebResources {
    static final String ORIGIN = "https://nxnative.invalid/";
    private static final Pattern DOCTYPE = Pattern.compile("(?is)^\\ufeff?\\s*<!doctype[^>]*>");
    private final File root, font;
    private final String top, script;
    static final class Resource {
        final int status;
        final String mime;
        final InputStream body;
        Resource(int status, String mime, InputStream body) { this.status = status; this.mime = mime; this.body = body; }
    }
    OfflineWebResources(File root, String path, File font, String script) throws IOException {
        this.root = root.getCanonicalFile(); this.font = font; this.script = script;
        int slash = path.indexOf('/'); this.top = slash > 0 ? path.substring(0, slash) : "";
    }
    static boolean local(URI uri) {
        return "https".equalsIgnoreCase(uri.getScheme()) && "nxnative.invalid".equalsIgnoreCase(uri.getHost())
            && uri.getUserInfo() == null && (uri.getPort() == -1 || uri.getPort() == 443);
    }
    private File file(String path) throws IOException {
        File selected = new File(root, path).getCanonicalFile();
        return selected.toPath().startsWith(root.toPath()) && selected.isFile() ? selected : null;
    }
    private static String mime(String name) {
        String ext = name.substring(name.lastIndexOf('.') + 1).toLowerCase(Locale.ROOT);
        switch (ext) {
            case "html": case "htm": return "text/html";
            case "css": return "text/css";
            case "js": return "text/javascript";
            case "json": return "application/json";
            case "png": return "image/png";
            case "jpg": case "jpeg": return "image/jpeg";
            case "gif": return "image/gif";
            case "webp": return "image/webp";
            case "svg": return "image/svg+xml";
            case "wav": return "audio/wav";
            case "mp3": return "audio/mpeg";
            case "ogg": return "audio/ogg";
            case "m4a": return "audio/mp4";
            case "mp4": return "video/mp4";
            case "webm": return "video/webm";
            case "woff": return "font/woff";
            case "woff2": return "font/woff2";
            case "ttf": return "font/ttf";
            case "otf": return "font/otf";
            case "txt": return "text/plain";
            case "xml": return "application/xml";
            default: return "application/octet-stream";
        }
    }
    Resource open(String url) {
        try {
            URI uri = new URI(url);
            if (!local(uri)) return missing();
            String rel = uri.getPath();
            if (rel == null || rel.indexOf('\0') >= 0 || rel.indexOf('\\') >= 0) return missing();
            while (rel.startsWith("/")) rel = rel.substring(1);
            // Reject actual parent traversal, including percent-encoded forms.
            // Browser-normalized ../ requests stay under the synthetic origin;
            // the fallback below finds the Apple document's own img/css folder.
            for (String part : rel.split("/")) if (part.equals("..")) return missing();
            File selected;
            if (rel.equals("__dm_font/nintendo_ext.ttf")) selected = font;
            else {
                selected = file(rel);
                if (selected == null && !top.isEmpty() && !rel.startsWith(top + "/")) selected = file(top + "/" + rel);
            }
            if (selected == null || !selected.isFile()) return missing();
            String type = mime(selected.getName());
            if (!type.equals("text/html")) return new Resource(200, type, new FileInputStream(selected));
            String html = new String(Files.readAllBytes(selected.toPath()), StandardCharsets.UTF_8);
            Matcher doctype = DOCTYPE.matcher(html);
            int at = doctype.find() ? doctype.end() : html.startsWith("\ufeff") ? 1 : 0;
            String injected = html.substring(0, at) + "<script>" + script.replace("</script", "<\\/script") + "</script>" + html.substring(at);
            return new Resource(200, type, new ByteArrayInputStream(injected.getBytes(StandardCharsets.UTF_8)));
        } catch (IOException | IllegalArgumentException | java.net.URISyntaxException error) { return missing(); }
    }
    private static Resource missing() { return new Resource(404, "text/plain", new ByteArrayInputStream(new byte[0])); }
}
