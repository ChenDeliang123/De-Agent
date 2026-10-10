const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function normalizeBackendUrl(url) {
  return url.replace(/\/+$/, "");
}

export async function listSessions(backendUrl) {
  const response = await fetch(`${normalizeBackendUrl(backendUrl)}/api/v1/sessions`);
  if (!response.ok) {
    throw new Error(`failed to list sessions: HTTP ${response.status}`);
  }
  return response.json();
}

export async function loadHistory(backendUrl, sessionId) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}/messages`,
  );
  if (!response.ok) {
    throw new Error(`failed to load session history: HTTP ${response.status}`);
  }
  return response.json();
}

export async function compactSession(backendUrl, sessionId, cwd = process.cwd()) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}/compact`,
    {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: encoder.encode(JSON.stringify({ cwd })),
    },
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `compact request failed: HTTP ${response.status}`);
  }
  return body;
}

export async function renameSession(backendUrl, sessionId, title) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}`,
    {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: encoder.encode(JSON.stringify({ title })),
    },
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `rename failed: HTTP ${response.status}`);
  }
  return body;
}

export async function deleteSession(backendUrl, sessionId) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}`,
    { method: "DELETE" },
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `delete failed: HTTP ${response.status}`);
  }
  return body;
}

export async function cloneSession(backendUrl, sessionId) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}/clone`,
    { method: "POST" },
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `clone failed: HTTP ${response.status}`);
  }
  return body;
}

export async function sessionStats(backendUrl, sessionId) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/sessions/${encodeURIComponent(sessionId)}/stats`,
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `stats request failed: HTTP ${response.status}`);
  }
  return body;
}

export async function listTools(backendUrl) {
  const response = await fetch(`${normalizeBackendUrl(backendUrl)}/api/v1/tools`);
  if (!response.ok) {
    throw new Error(`failed to list tools: HTTP ${response.status}`);
  }
  return response.json();
}

export async function health(backendUrl) {
  const response = await fetch(`${normalizeBackendUrl(backendUrl)}/health`);
  if (!response.ok) {
    throw new Error(`health check failed: HTTP ${response.status}`);
  }
  return response.json();
}

export async function cancelChat(backendUrl, sessionId) {  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/chat/cancel`,
    {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: encoder.encode(JSON.stringify({ session_id: sessionId })),
    },
  );
  if (!response.ok) {
    throw new Error(`cancel request failed: HTTP ${response.status}`);
  }
  return response.json();
}

export async function streamChat(
  backendUrl,
  sessionId,
  message,
  onEvent,
  cwd = process.cwd(),
  signal,
) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/chat/stream`,
    {
      method: "POST",
      headers: { "content-type": "application/json" },
      signal,
      body: encoder.encode(
        JSON.stringify({
          ...(sessionId ? { session_id: sessionId } : {}),
          cwd,
          message,
        }),
      ),
    },
  );

  if (!response.ok || !response.body) {
    throw new Error(`stream request failed: HTTP ${response.status} ${await response.text()}`);
  }

  const reader = response.body.getReader();
  let buffer = "";
  let donePayload = null;

  while (true) {
    const { value, done } = await reader.read();
    if (done) {
      break;
    }
    buffer += decoder.decode(value, { stream: true });

    let boundary;
    while ((boundary = buffer.indexOf("\n\n")) !== -1) {
      const block = buffer.slice(0, boundary);
      buffer = buffer.slice(boundary + 2);

      let eventName = "message";
      let data = null;
      for (const line of block.split("\n")) {
        if (line.startsWith("event: ")) {
          eventName = line.slice(7).trim();
        } else if (line.startsWith("data: ")) {
          data = JSON.parse(line.slice(6));
        }
      }
      if (eventName === "done") {
        donePayload = data;
      }
      onEvent?.({ event: eventName, data });
    }
  }
  return donePayload;
}

export async function effectivePermission(backendUrl, path) {
  const url = `${normalizeBackendUrl(backendUrl)}/api/v1/permissions/effective?path=${encodeURIComponent(path)}`;
  const response = await fetch(url);
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `permission request failed: HTTP ${response.status}`);
  }
  return body;
}

export async function setPermission(backendUrl, path, mode) {
  const response = await fetch(`${normalizeBackendUrl(backendUrl)}/api/v1/permissions`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: encoder.encode(JSON.stringify({ path, mode })),
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `set permission failed: HTTP ${response.status}`);
  }
  return body;
}

export async function resetPermission(backendUrl, path) {
  const response = await fetch(
    `${normalizeBackendUrl(backendUrl)}/api/v1/permissions?path=${encodeURIComponent(path)}`,
    { method: "DELETE" },
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `reset permission failed: HTTP ${response.status}`);
  }
  return body;
}

export async function listPermissions(backendUrl) {
  const response = await fetch(`${normalizeBackendUrl(backendUrl)}/api/v1/permissions`);
  const body = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(body.error ?? `list permissions failed: HTTP ${response.status}`);
  }
  return body;
}
