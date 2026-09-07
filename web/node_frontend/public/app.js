const sessionList = document.getElementById("session-list");
const messagesBox = document.getElementById("messages");
const chatForm = document.getElementById("chat-form");
const messageInput = document.getElementById("message-input");
const newSessionButton = document.getElementById("new-session");

let currentSessionId = null;

function appendMessage(role, content) {
  const element = document.createElement("div");
  element.className = `message ${role}`;
  element.textContent = content;
  messagesBox.appendChild(element);
  messagesBox.scrollTop = messagesBox.scrollHeight;
  return element;
}

function renderHistory(messages) {
  messagesBox.replaceChildren();
  for (const message of messages) {
    if (message.role === "user" || message.role === "assistant") {
      appendMessage(message.role, message.content);
    }
  }
}

async function loadSessions() {
  const response = await fetch("/api/sessions");
  const sessions = await response.json();
  sessionList.replaceChildren();
  for (const sessionId of sessions) {
    const item = document.createElement("li");
    item.textContent = sessionId.slice(0, 8);
    item.addEventListener("click", async () => {
      currentSessionId = sessionId;
      document.querySelectorAll("#session-list li").forEach((node) => node.classList.remove("active"));
      item.classList.add("active");
      const historyResponse = await fetch(`/api/sessions/${encodeURIComponent(sessionId)}/messages`);
      renderHistory(await historyResponse.json());
    });
    sessionList.appendChild(item);
  }
}

function parseSseBlocks(buffer) {
  const blocks = [];
  let start = 0;
  while (start < buffer.length) {
    const end = buffer.indexOf("\n\n", start);
    if (end === -1) break;
    blocks.push(buffer.slice(start, end));
    start = end + 2;
  }
  return { blocks, rest: buffer.slice(start) };
}

async function sendMessage(content) {
  appendMessage("user", content);
  const assistantElement = appendMessage("assistant", "");

  const response = await fetch("/api/chat/stream", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ session_id: currentSessionId, message: content }),
  });

  if (!response.ok || !response.body) {
    assistantElement.textContent = "请求失败：" + (await response.text());
    return;
  }

  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let donePayload = null;

  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });
    const { blocks, rest } = parseSseBlocks(buffer);
    buffer = rest;

    for (const block of blocks) {
      const eventLine = block.split("\n").find((line) => line.startsWith("event: "));
      const dataLine = block.split("\n").find((line) => line.startsWith("data: "));
      if (!dataLine) continue;
      const event = eventLine ? eventLine.slice(7) : "message";
      const data = JSON.parse(dataLine.slice(6));

      if (event === "content") {
        assistantElement.textContent += data.content ?? "";
        messagesBox.scrollTop = messagesBox.scrollHeight;
      } else if (event === "done") {
        donePayload = data;
      } else if (event === "error") {
        assistantElement.textContent = "错误：" + (data.error ?? "unknown");
      }
    }
  }

  if (donePayload?.session_id) {
    currentSessionId = donePayload.session_id;
    await loadSessions();
  }
}

newSessionButton.addEventListener("click", () => {
  currentSessionId = null;
  messagesBox.replaceChildren();
  messageInput.focus();
});

chatForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const content = messageInput.value.trim();
  if (!content) return;
  messageInput.value = "";
  sendMessage(content).catch((error) => {
    appendMessage("assistant", "请求异常：" + error.message);
  });
});

loadSessions();
