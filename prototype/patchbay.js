"use strict";

// UI concept only. No audio API, installed driver, network peer, or native IPC.
const BANK = 32;
const descriptions = {
  ks: "KS capture/render endpoint. The real engine must discover supported pins, formats, and available instances.",
  cable: "Logical app send/return pair. App playback goes to the DAW; a separate recording endpoint receives the DAW return.",
  bridge: "Secondary ASIO application. Its outputs feed the primary DAW inputs; it receives primary DAW outputs on its inputs.",
  lan: "Illustrative LAN peer. Receive and transmit have independent queues; jitter and clock adaptation add path delay.",
  asio: "Existing hardware ASIO driver hosted by the engine. The primary DAW opens WinHookAudio ASIO."
};
const kinds = { ks: "KERNEL STREAMING", cable: "APP CABLE PAIR", bridge: "ASIO CLIENT BRIDGE", lan: "LAN / VBAN CANDIDATE", asio: "HARDWARE ASIO" };
function device(id, name, kind, inputs, outputs, to, from) {
  return { id, name, kind, inputs, outputs, to, from, rate: 48000, format: kind === "bridge" ? "float32" : "pcm24in32", buffer: kind === "lan" ? 512 : 128, jitter: 20 };
}
function initial() {
  return { conceptOnly: true, schemaVersion: 1, channelBank: BANK, rate: 48000, block: 128, asioFormat: "float32", master: "ks-output", devices: [
    device("ks-input", "Microphone", "ks", 1, 0, 1, 0),
    device("ks-output", "Monitor output", "ks", 0, 2, 0, 1),
    device("cable-1", "App send / return", "cable", 2, 2, 3, 3),
    device("bridge-1", "ASIO application", "bridge", 2, 2, 5, 5),
    device("lan-1", "Studio computer", "lan", 2, 2, 7, 7)
  ] };
}
let state = initial();
let selected = state.devices[0].id;
let serial = 1;
const $ = id => document.getElementById(id);
const range = (start, width) => width === 1 ? String(start) : `${start}–${start + width - 1}`;
const selectedDevice = () => state.devices.find(d => d.id === selected);
function message(text, error = false) { $("status").textContent = text; $("status").classList.toggle("error", error); }
function validate() {
  const occupied = new Map();
  for (const d of state.devices) {
    for (const [key, width] of [["to", d.inputs], ["from", d.outputs]]) {
      const start = d[key];
      if (!Number.isInteger(start) || start < 0 || (start && (!width || start + width - 1 > BANK))) return `${d.name}: invalid channel range.`;
    }
    if (!d.to) continue;
    for (let ch = d.to; ch < d.to + d.inputs; ch++) {
      if (occupied.has(ch)) return `Input ${ch} overlaps: ${occupied.get(ch)} and ${d.name}. Choose separate DAW inputs.`;
      occupied.set(ch, d.name);
    }
  }
  return null;
}
function changed() {
  const error = validate();
  message(error || "Draft changed. Apply to update the preview configuration.", !!error);
  $("apply").disabled = !!error;
  $("export").disabled = !!error;
  updateSummary();
}
function create(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text !== undefined) el.textContent = text;
  return el;
}
function renderGrid() {
  const head = $("grid").tHead;
  const body = $("grid").tBodies[0];
  head.replaceChildren(); body.replaceChildren();
  $("grid").style.minWidth = `${170 + state.devices.length * 210}px`;
  const header = create("tr");
  header.append(create("th", "", "DAW CHANNEL BANK"));
  for (const d of state.devices) {
    const th = create("th", d.id === selected ? "device-active" : "");
    th.scope = "col";
    th.append(create("span", "device-kind", kinds[d.kind]));
    const button = create("button", "", d.name);
    button.setAttribute("aria-label", `Inspect ${d.name}`);
    button.addEventListener("click", () => { selected = d.id; renderGrid(); renderInspector(); });
    th.append(button, create("div", "device-meta", `${d.inputs} to DAW / ${d.outputs} from DAW · example`));
    header.append(th);
  }
  head.append(header);
  for (const key of ["to", "from"]) {
    const tr = create("tr", key === "to" ? "to-daw" : "from-daw");
    const label = create("th"); label.scope = "row";
    label.append(create("span", "row-title", key === "to" ? "→ To DAW" : "← From DAW"), create("span", "row-detail", key === "to" ? "Source → DAW inputs" : "DAW outputs → destination"));
    tr.append(label);
    for (const d of state.devices) {
      const td = create("td");
      const width = key === "to" ? d.inputs : d.outputs;
      if (!width) { td.append(create("div", "unavailable", "No channels in this direction")); tr.append(td); continue; }
      td.append(create("p", "channel-label", `DEVICE CH ${range(1, width)}`));
      const select = create("select");
      select.setAttribute("aria-label", `${d.name} ${key === "to" ? "to DAW input" : "from DAW output"}`);
      select.add(new Option("Unassigned", "0"));
      for (let start = 1; start <= BANK - width + 1; start++) select.add(new Option(`DAW ${key === "to" ? "In" : "Out"} ${range(start, width)}`, String(start)));
      select.value = String(d[key]);
      const note = create("p", "cell-note");
      const updateNote = () => { note.textContent = d[key] ? `${range(1, width)} ${key === "to" ? "→" : "←"} ${range(d[key], width)}` : "Disconnected in preview"; };
      updateNote();
      select.addEventListener("change", () => { d[key] = Number(select.value); updateNote(); changed(); });
      td.append(select, note); tr.append(td);
    }
    body.append(tr);
  }
}
function renderMaster() {
  const master = $("master"); master.replaceChildren();
  master.add(new Option("Internal clock", "internal"));
  for (const d of state.devices.filter(d => d.outputs && ["ks", "asio"].includes(d.kind))) master.add(new Option(`${d.name} · example`, d.id));
  master.value = state.master;
}
function renderInspector() {
  const d = selectedDevice();
  $("inspector-title").textContent = d.name;
  $("device-description").textContent = descriptions[d.kind];
  $("endpoint-rate").value = String(d.rate);
  $("endpoint-format").value = d.format;
  $("endpoint-buffer").value = String(d.buffer);
  $("jitter").value = String(d.jitter);
  $("jitter-label").hidden = d.kind !== "lan";
  $("clock-note").textContent = d.id === state.master
    ? "Proposed master device. Session and device rates must agree when a real device is opened."
    : `Independent clock: adaptive resampling proposed. ${d.rate === state.rate ? "Matching nominal rates do not establish clock synchronization." : "This endpoint also needs nominal sample-rate conversion."}`;
}
function updateSummary() {
  $("block-time").textContent = `${(state.block * 1000 / state.rate).toFixed(2)} ms`;
  const inputs = state.devices.reduce((sum, d) => sum + (d.to ? d.inputs : 0), 0);
  const outputs = state.devices.reduce((sum, d) => sum + (d.from ? d.outputs : 0), 0);
  $("input-count").textContent = `${inputs} assigned ch`;
  $("output-count").textContent = `${outputs} destination ch`;
  $("route-count").textContent = `${state.devices.length} example devices · 32 × 32 ASIO bank`;
  for (const button of document.querySelectorAll("[data-profile]")) {
    const active = Number(button.dataset.profile) === state.block;
    button.classList.toggle("selected", active); button.setAttribute("aria-pressed", String(active));
  }
}
function renderAll() {
  $("rate").value = String(state.rate); $("block").value = String(state.block);
  renderMaster(); renderGrid(); renderInspector(); updateSummary();
}
$("rate").addEventListener("change", e => { state.rate = Number(e.target.value); renderInspector(); changed(); });
$("block").addEventListener("change", e => { state.block = Number(e.target.value); changed(); });
$("master").addEventListener("change", e => { state.master = e.target.value; renderInspector(); changed(); });
for (const button of document.querySelectorAll("[data-profile]")) button.addEventListener("click", () => { state.block = Number(button.dataset.profile); $("block").value = String(state.block); changed(); });
for (const [id, key] of [["endpoint-rate", "rate"], ["endpoint-format", "format"], ["endpoint-buffer", "buffer"], ["jitter", "jitter"]]) {
  $(id).addEventListener("change", e => { selectedDevice()[key] = key === "format" ? e.target.value : Number(e.target.value); renderInspector(); changed(); });
}
$("add").addEventListener("click", () => {
  const kind = $("device-type").value;
  const names = { ks: "KS device", cable: "App cable pair", bridge: "ASIO bridge", lan: "LAN peer", asio: "Hardware ASIO" };
  const d = device(`added-${serial}`, `${names[kind]} ${serial++}`, kind, 2, 2, 0, 0);
  state.devices.push(d); selected = d.id; renderAll(); changed();
});
$("apply").addEventListener("click", () => {
  const error = validate();
  if (error) { message(error, true); return; }
  message("Preview configuration applied. Audio engine and drivers are not implemented.");
});
$("reset").addEventListener("click", () => { state = initial(); selected = state.devices[0].id; serial = 1; renderAll(); $("apply").disabled = false; $("export").disabled = false; message("Example configuration reset. No system audio is changed."); });
$("export").addEventListener("click", () => {
  const error = validate(); if (error) { message(error, true); return; }
  const url = URL.createObjectURL(new Blob([JSON.stringify(state, null, 2)], { type: "application/json" }));
  const link = create("a"); link.href = url; link.download = "winhookaudio-concept.json";
  document.body.append(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  message("Exported design configuration. This is not an installable audio-driver preset.");
});
renderAll();
