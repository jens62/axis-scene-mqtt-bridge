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
const prefixField = document.querySelector('[data-param="TopicPrefix"]');

const motionChecks = Array.from(document.querySelectorAll("[data-motion]"));
const motionField = document.querySelector('[data-param="MotionEvents"]');

let loaded = {};  // values as read from the camera, to send only what changed

function setStatus(text, kind) {
  status.textContent = text;
  status.className = kind || "";
}

// The camera capitalizes the group name ("root.Axis_scene_mqtt_bridge"), so match case-insensitively.
function parseParams(text, group) {
  const values = {};
  const prefix = (group + ".").toLowerCase();
  for (const line of text.split(/\r?\n/)) {
    const eq = line.indexOf("=");
    if (eq > 0 && line.slice(0, prefix.length).toLowerCase() === prefix)
      values[line.slice(prefix.length, eq)] = line.slice(eq + 1);
  }
  return values;
}

async function listGroup(group) {
  const res = await fetch(PARAM_CGI + "?action=list&group=" + encodeURIComponent(group),
                          { credentials: "same-origin" });
  if (!res.ok) throw new Error("HTTP " + res.status);
  return parseParams(await res.text(), group);
}

// Shows the topic prefix the app uses when the field is empty. Best effort only.
async function showDefaultPrefix() {
  try {
    const serial = (await listGroup("root.Properties.System"))["SerialNumber"];
    if (serial) prefixField.placeholder = "axis/" + serial + "/bridge";
  } catch (err) {
    // keep the generic placeholder
  }
}

function motionList() {
  return motionField.value.split(",").map((s) => s.trim()).filter((s) => s !== "");
}

function motionChecksFromText() {
  const list = motionList();
  for (const box of motionChecks) box.checked = list.includes(box.dataset.motion);
}

function motionTextFromChecks(changed) {
  const list = motionList().filter((s) => s !== changed.dataset.motion);
  if (changed.checked) list.push(changed.dataset.motion);
  motionField.value = list.join(", ");
}

function readField(el) {
  return el.type === "checkbox" ? (el.checked ? "yes" : "no") : el.value.trim();
}

function writeField(el, value) {
  if (el.type === "checkbox") {
    el.checked = value === "yes";
    return;
  }
  if (el.tagName === "SELECT" && value !== "" && !Array.from(el.options).some((o) => o.value === value)) {
    const option = document.createElement("option");  // keep a stored value that is not in the list
    option.value = value;
    option.textContent = value;
    el.appendChild(option);
  }
  el.value = value;
  if (el === motionField) motionChecksFromText();
}

async function load() {
  setStatus("Loading…");
  try {
    loaded = await listGroup(GROUP);
    if (!("MqttHost" in loaded)) throw new Error("parameters not found, is the app installed?");
    for (const el of fields) writeField(el, loaded[el.dataset.param] ?? "");
    password.value = "";
    clearPassword.checked = false;
    setStatus("");
    showDefaultPrefix();
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
         num("MinIntervalMs", 0, 3600000) || num("AudioHoldSec", 0, 3600) || num("MotionHoldSec", 0, 3600) || num("ClearTimeoutSec", 1, 86400);
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

// defaults.json is generated from manifest.json when the package is built.
async function resetToDefaults() {
  if (!confirm("Reset all settings, including the MQTT broker, to the defaults?")) return;
  try {
    const res = await fetch("defaults.json", { credentials: "same-origin" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    const defaults = await res.json();
    for (const el of fields) writeField(el, defaults[el.dataset.param] ?? "");
    password.value = "";
    clearPassword.checked = true;
    setStatus("Defaults loaded. Press Save to apply them.");
  } catch (err) {
    setStatus("Could not load the defaults: " + err.message, "error");
  }
}

// The password is not exported.
function exportSettings() {
  const settings = {};
  for (const el of fields) settings[el.dataset.param] = readField(el);
  const blob = new Blob([JSON.stringify(settings, null, 2) + "\n"], { type: "application/json" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = "scene-mqtt-bridge-settings.json";
  link.click();
  URL.revokeObjectURL(link.href);
  setStatus("Settings exported (without the password).");
}

async function importSettings(file) {
  try {
    const settings = JSON.parse(await file.text());
    let count = 0;
    for (const el of fields) {
      const key = el.dataset.param;
      if (typeof settings[key] === "string") {
        writeField(el, settings[key]);
        count++;
      }
    }
    if (count === 0) throw new Error("no known settings in this file");
    setStatus("Imported " + count + " settings. Press Save to apply them.");
  } catch (err) {
    setStatus("Could not import: " + err.message, "error");
  }
}

for (const box of motionChecks) box.addEventListener("change", () => motionTextFromChecks(box));
motionField.addEventListener("input", motionChecksFromText);
document.getElementById("reset").addEventListener("click", resetToDefaults);
document.getElementById("export").addEventListener("click", exportSettings);
const importFile = document.getElementById("import-file");
document.getElementById("import").addEventListener("click", () => importFile.click());
importFile.addEventListener("change", () => {
  if (importFile.files.length > 0) importSettings(importFile.files[0]);
  importFile.value = "";
});

form.addEventListener("submit", save);
document.getElementById("reload").addEventListener("click", load);
document.addEventListener("DOMContentLoaded", load);
