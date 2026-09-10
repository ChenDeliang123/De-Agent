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

export async function cancelChat(backendUrl, sessionId) {
  const response = await fetch(
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
