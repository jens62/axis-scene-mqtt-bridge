"use strict";

// The camera's web server runs this page and param.cgi on the same origin, so the
// browser's existing camera login is used. Everything is written with textContent
// or .value, never innerHTML.
const GROUP = "root.axis_scene_mqtt_bridge";
const PARAM_CGI = "/axis-cgi/param.cgi";

const form = document.getElementById("form");
const status = document.getElementById("status");
const password = document.getElementById("password");
const clearPassword = document.getElementById("clear-password");
const fields = Array.from(document.querySelectorAll("[data-param]"));

let loaded = {};  // values as read from the camera, to send only what changed

function setStatus(text, kind) {
  status.textContent = text;
  status.className = kind || "";
}

function parseParams(text) {
  const values = {};
  const prefix = GROUP + ".";
  for (const line of text.split(/\r?\n/)) {
    const eq = line.indexOf("=");
    if (eq > 0 && line.startsWith(prefix))
      values[line.slice(prefix.length, eq)] = line.slice(eq + 1);
  }
  return values;
}

function readField(el) {
  return el.type === "checkbox" ? (el.checked ? "yes" : "no") : el.value.trim();
}

function writeField(el, value) {
  if (el.type === "checkbox") el.checked = value === "yes";
  else el.value = value;
}

async function load() {
  setStatus("Loading…");
  try {
    const res = await fetch(PARAM_CGI + "?action=list&group=" + encodeURIComponent(GROUP),
                            { credentials: "same-origin" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    loaded = parseParams(await res.text());
    if (!("MqttHost" in loaded)) throw new Error("parameters not found, is the app installed?");
    for (const el of fields) writeField(el, loaded[el.dataset.param] ?? "");
    password.value = "";
    clearPassword.checked = false;
    setStatus("");
  } catch (err) {
    setStatus("Could not load settings: " + err.message, "error");
  }
}

function validate(changes) {
  const num = (key, min, max) => {
    if (!(key in changes)) return null;
    const n = Number(changes[key]);
    return Number.isFinite(n) && n >= min && n <= max ? null : key + " must be a number from " + min + " to " + max;
  };
  return num("MqttPort", 1, 65535) || num("MoveThreshold", 0, 10) ||
         num("MinIntervalMs", 0, 3600000) || num("ClearTimeoutSec", 1, 86400);
}

async function save(event) {
  event.preventDefault();

  const changes = {};
  for (const el of fields) {
    const key = el.dataset.param;
    const value = readField(el);
    if (value !== (loaded[key] ?? "")) changes[key] = value;
  }
  if (clearPassword.checked) changes.MqttPassword = "";
  else if (password.value !== "") changes.MqttPassword = password.value;

  if (Object.keys(changes).length === 0) {
    setStatus("Nothing changed.");
    return;
  }
  const problem = validate(changes);
  if (problem) {
    setStatus(problem, "error");
    return;
  }

  const body = new URLSearchParams({ action: "update" });
  for (const [key, value] of Object.entries(changes)) body.set(GROUP + "." + key, value);

  setStatus("Saving…");
  try {
    const res = await fetch(PARAM_CGI, {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    });
    const text = (await res.text()).trim();
    if (!res.ok || !text.startsWith("OK")) throw new Error(text || "HTTP " + res.status);
    setStatus("Saved. The application restarts to apply the settings.", "ok");
    setTimeout(load, 2000);
  } catch (err) {
    setStatus("Could not save: " + err.message, "error");
  }
}

form.addEventListener("submit", save);
document.getElementById("reload").addEventListener("click", load);
document.addEventListener("DOMContentLoaded", load);
