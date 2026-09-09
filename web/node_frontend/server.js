import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { log } from "./logger.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const publicDir = path.join(__dirname, "public");

const mimeTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
};

export function createHttpServer(
  backendBaseUrl = process.env.AIAGENT_BACKEND_URL || "http://127.0.0.1:18081",
) {
  return http.createServer((request, response) => {
    const requestUrl = new URL(request.url, "http://localhost");
    const pathname = requestUrl.pathname;
    const started = Date.now();
    log("info", "web", `event=request_start method=${request.method} path=${pathname}`);
    response.on("finish", () => {
      log(
        "info",
        "web",
        `event=request_end method=${request.method} path=${pathname} status=${response.statusCode} duration_ms=${Date.now() - started}`,
      );
    });

    if (pathname.startsWith("/api/")) {
      proxyToBackend(request, response, backendBaseUrl, backendApiPath(pathname));
      return;
    }

    serveStatic(response, pathname);
  });
}

function backendApiPath(pathname) {
  if (pathname === "/api/chat" || pathname === "/api/chat/stream") {
    return "/api/v1" + pathname.slice("/api".length);
  }
  if (pathname.startsWith("/api/sessions")) {
    return "/api/v1" + pathname.slice("/api".length);
  }
  return pathname;
}

function proxyToBackend(request, response, backendBaseUrl, pathname) {
  const target = new URL(pathname, backendBaseUrl);
  const headers = { ...request.headers };
  delete headers.host;
  delete headers.connection;

  const upstream = http.request(
    {
      protocol: target.protocol,
      hostname: target.hostname,
      port: target.port || (target.protocol === "https:" ? 443 : 80),
      path: target.pathname + target.search,
      method: request.method,
      headers,
    },
    (upstreamResponse) => {
      const responseHeaders = { ...upstreamResponse.headers };
      delete responseHeaders["transfer-encoding"];
      delete responseHeaders.connection;
      response.writeHead(upstreamResponse.statusCode ?? 500, responseHeaders);
      upstreamResponse.pipe(response);
    },
  );

  upstream.on("error", (error) => {
    console.error(`proxy error for ${pathname}:`, error.message);
    log("error", "web", `event=proxy_error path=${pathname} message=${error.message}`);
    if (!response.headersSent) {
      response.writeHead(502, { "content-type": "application/json" });
    }
    response.end(JSON.stringify({ error: "backend unavailable" }));
  });

  request.pipe(upstream);
}

function serveStatic(response, pathname) {
  let relative = pathname === "/" ? "index.html" : pathname.replace(/^\/+/, "");
  const filePath = path.resolve(publicDir, relative);
  if (!filePath.startsWith(publicDir + path.sep) && filePath !== path.join(publicDir, "index.html")) {
    response.writeHead(403);
    response.end("forbidden");
    return;
  }

  fs.readFile(filePath, (error, data) => {
    if (error) {
      response.writeHead(404);
      response.end("not found");
      return;
    }
    const extension = path.extname(filePath).toLowerCase();
    response.writeHead(200, { "content-type": mimeTypes[extension] || "application/octet-stream" });
    response.end(data);
  });
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  const port = Number(process.env.PORT || 3000);
  const server = createHttpServer();
  server.listen(port, () => {
    console.log(`ai-agent web listening on http://127.0.0.1:${port}`);
    console.log(`backend: ${process.env.AIAGENT_BACKEND_URL || "http://127.0.0.1:18081"}`);
    log("info", "web", `event=server_start port=${port}`);
  });
}
