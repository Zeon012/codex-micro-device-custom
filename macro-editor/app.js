const initialControls = [
  ['Task 1', 6, 0, 0, 0, 0], ['Task 2', 10, 0, 0, 0, 0],
  ['Task 3', 4, 0, 0, 0, 0], ['Task 4', 7, 0, 0, 0, 0],
  ['Task 5', 11, 0, 0, 0, 0], ['Task 6', 14, 0, 0, 0, 0],
  ['Fast mode', 5, 0, 0, 0, 0], ['Approve', 8, 0, 0, 0, 0],
  ['Reject', 12, 0, 0, 0, 0], ['Continue task', 15, 0, 0, 0, 0],
  ['Push to talk A', 9, 0, 0, 0, 0], ['Push to talk B', 13, 0, 0, 0, 0],
  ['Send', 16, 0, 0, 0, 0], ['Encoder press', 12, 0, 0, 0, 0],
  ['Encoder clockwise', 'ROTATE CW', 0, 0, 0, 0],
  ['Encoder counter-clockwise', 'ROTATE CCW', 0, 0, 0, 0]
].map(([name, gpio, key, modifier, behavior, longKey]) => ({ name, gpio, key, modifier, behavior, longKey }));
const defaultControls = initialControls.map(control => ({ ...control }));
const layers = [
  defaultControls.map(control => ({ ...control })),
  defaultControls.map(control => ({ ...control }))
];
let controls = layers[0];
let activeLayer = 0;

const keyOptions = [
  [0, 'None'], [40, 'Enter'], [41, 'Escape'], [43, 'Tab'], [44, 'Space'],
  [240, 'Fn'],
  ...Array.from({ length: 26 }, (_, i) => [4 + i, String.fromCharCode(65 + i)]),
  ...Array.from({ length: 10 }, (_, i) => [30 + i, String(i)]),
  ...Array.from({ length: 12 }, (_, i) => [58 + i, `F${i + 1}`]),
  ...Array.from({ length: 12 }, (_, i) => [104 + i, `F${i + 13}`])
];
const modifierOptions = [[0, 'None'], [128, 'Ctrl'], [2, 'Shift'], [4, 'Alt'], [8, 'Win']];
const behaviorOptions = [[0, 'Tap'], [1, 'Hold'], [2, 'Long press'], [3, 'Short + long']];
const mediaOptions = [['Play / Pause', 241], ['Next track', 242], ['Previous track', 243], ['Volume up', 244], ['Volume down', 245], ['Mute', 246], ['Brightness up', 247], ['Brightness down', 248]];
const macroOptions = Array.from({ length: 8 }, (_, index) => [`Macro ${index + 1}`, 224 + index]);
const microsoftShortcutModifier = 128 | 8 | 4 | 2;
const appOptions = [
  ['LinkedIn', 15], ['OneNote', 17], ['Outlook', 18], ['PowerPoint', 19],
  ['Teams', 23], ['Word', 26], ['Excel', 27], ['Yammer / Viva', 28]
].map(([label, key]) => [label, key, microsoftShortcutModifier]);
const windowsShortcutOptions = [
  ['Win + X menu', 27, 8], ['Win + R Run', 21, 8], ['Win + E Explorer', 8, 8],
  ['Win + I Settings', 12, 8], ['Win + D Desktop', 7, 8], ['Win + L Lock', 15, 8]
];
const quickLinkOptions = [
  ['Terminal (Win+X, I)', 232], ['Admin Terminal (Win+X, A)', 233],
  ['Task Manager (Win+X, T)', 234], ['Disk Management (Win+X, K)', 235],
  ['Device Manager (Win+X, M)', 236], ['Computer Management (Win+X, G)', 237],
  ['File Explorer (Win+X, E)', 238], ['Run (Win+X, R)', 239]
];
const paletteDefinitions = {
  BASIC: ['None', 'Esc', 'Tab', 'Enter', 'Space', 'Backspace', 'Delete', 'Ctrl', 'Shift', 'Alt', 'Win', 'Fn', ...Array.from({ length: 12 }, (_, i) => `F${i + 1}`), ...Array.from({ length: 8 }, (_, i) => `F${i + 13}`), ...Array.from({ length: 26 }, (_, i) => String.fromCharCode(65 + i))],
  MEDIA: mediaOptions.map(([label]) => label),
  MACRO: macroOptions.map(([label]) => label),
  LAYERS: ['Fn', 'Keymap 0', 'Keymap 1'],
  SPECIAL: ['None', 'Esc', 'Tab', 'Enter', 'Space', 'Backspace', 'Delete'],
  LIGHTING: ['Brightness up', 'Brightness down'],
  APPS: [...appOptions, ...windowsShortcutOptions].map(([label]) => label),
  QUICKLINK: quickLinkOptions.map(([label]) => label)
};

const grid = document.querySelector('#macroGrid');
const connectButton = document.querySelector('#connectButton');
const saveButton = document.querySelector('#saveButton');
const resetButton = document.querySelector('#resetButton');
const statusText = document.querySelector('#statusText');
const statusDot = document.querySelector('#statusDot');
const dirtyState = document.querySelector('#dirtyState');
const portName = document.querySelector('#portName');
const keymapPreview = document.querySelector('#keymapPreview');
const selectedName = document.querySelector('#selectedName');
const selectedSummary = document.querySelector('#selectedSummary');
const selectedTest = document.querySelector('#selectedTest');
const editMacroButton = document.querySelector('#editMacroButton');
const keyTesterToggle = document.querySelector('#keyTesterToggle');
const keyTesterClear = document.querySelector('#keyTesterClear');
const keyTesterLog = document.querySelector('#keyTesterLog');
const calibrationToggle = document.querySelector('#calibrationToggle');
const calibrationClear = document.querySelector('#calibrationClear');
const calibrationStatus = document.querySelector('#calibrationStatus');
const calibrationLog = document.querySelector('#calibrationLog');
const macroDialog = document.querySelector('#macroDialog');
const macroInput = document.querySelector('#macroInput');
const macroInputLabel = document.querySelector('#macroInputLabel');
const macroInputHint = document.querySelector('#macroInputHint');
const macroSave = document.querySelector('#macroSave');
const macroCancel = document.querySelector('#macroCancel');
const paletteKeys = document.querySelector('.palette-keys');
let port;
let writer;
let reader;
let pendingLines = [];
let changed = false;
let selectedIndex = 0;
let keyTestEnabled = false;
let calibrationEnabled = false;
let writeQueue = Promise.resolve();

function options(values, selected) {
  return values.map(([value, label]) => `<option value="${value}" ${value === selected ? 'selected' : ''}>${label}</option>`).join('');
}

function keyLabel(value) {
  return keyOptions.find(([key]) => key === value)?.[1]
    || mediaOptions.find(([, key]) => key === value)?.[0]
    || macroOptions.find(([, key]) => key === value)?.[0]
    || quickLinkOptions.find(([, key]) => key === value)?.[0]
    || `Key ${value}`;
}

function modifierLabel(value) {
  if (value === 0) return 'None';
  return modifierOptions.slice(1)
    .filter(([key]) => (value & key) === key)
    .map(([, label]) => label)
    .join(' + ') || 'None';
}

function directionIcon(clockwise) {
  const path = clockwise
    ? '<path d="M5 8a8 8 0 1 1-1 7"/><path d="M5 3v5h5"/>'
    : '<path d="M19 8a8 8 0 1 0 1 7"/><path d="M19 3v5h-5"/>';
  return `<svg viewBox="0 0 24 24" aria-hidden="true">${path}</svg>`;
}

function renderKeymap() {
  const symbols = ['1', '2', '3', '4', '5', '6', '⚡', '✓', '✕', '↗', 'MIC', 'MIC', '⚙'];
  const positions = [
    'grid-column: 2; grid-row: 1', 'grid-column: 3; grid-row: 1',
    'grid-column: 1; grid-row: 2', 'grid-column: 2; grid-row: 2',
    'grid-column: 3; grid-row: 2', 'grid-column: 4; grid-row: 2',
    'grid-column: 1; grid-row: 3', 'grid-column: 2; grid-row: 3',
    'grid-column: 3; grid-row: 3', 'grid-column: 4; grid-row: 3',
    'grid-column: 2 / span 2; grid-row: 4', 'grid-column: 2 / span 2; grid-row: 4',
    'grid-column: 4; grid-row: 4'
  ];
  const keyMarkup = controls.slice(0, 13).map((control, index) => index === 11 ? '' : `
    <button class="map-key ${index === selectedIndex ? 'selected' : ''} ${index === 10 || index === 11 ? 'mic-key' : ''}" style="${positions[index]}" data-map-index="${index}">
      <span class="key-symbol">${symbols[index]}</span><br><small>${keyLabel(control.key)}</small>
    </button>`).join('');
  const encoder = controls[13];
  keymapPreview.innerHTML = `<div class="encoder-cluster" style="grid-column: 1; grid-row: 1 / span 2">
      <button class="map-direction ${selectedIndex === 14 ? 'selected' : ''}" data-map-index="14" title="Clockwise">${directionIcon(true)}</button>
      <button class="map-knob ${selectedIndex === 13 ? 'selected' : ''}" data-map-index="13">KNOB<br><small>${modifierLabel(encoder.modifier)} + ${keyLabel(encoder.key)}</small></button>
      <button class="map-direction ${selectedIndex === 15 ? 'selected' : ''}" data-map-index="15" title="Counter-clockwise">${directionIcon(false)}</button>
    </div>${keyMarkup}`;
  keymapPreview.querySelectorAll('[data-map-index]').forEach(button => button.addEventListener('click', () => {
    selectedIndex = Number(button.dataset.mapIndex);
    updateInspector();
    renderKeymap();
  }));
}

function updateInspector() {
  const control = controls[selectedIndex];
  selectedName.textContent = control.name;
  selectedSummary.textContent = `${modifierLabel(control.modifier)} + ${keyLabel(control.key)}`;
  editMacroButton.hidden = control.key < 224 || control.key > 231;
}

function render() {
  renderKeymap();
  updateInspector();
}

function bindCards() {
  grid.querySelectorAll('select').forEach(select => select.addEventListener('change', event => {
    const card = event.target.closest('.macro-card');
    const index = Number(card.dataset.index);
    selectedIndex = index;
    controls[index][event.target.dataset.field] = Number(event.target.value);
    card.classList.add('changed');
    setDirty(true);
    if (event.target.dataset.field === 'behavior') {
      card.querySelector('.long-field').style.opacity = controls[index].behavior === 0 ? '.45' : '1';
    }
    sendSetting(index);
    updateInspector();
    renderKeymap();
  }));
  grid.querySelectorAll('[data-test]').forEach(button => button.addEventListener('click', () => {
    send(`TEST,${button.dataset.test}`);
  }));
  grid.querySelectorAll('.long-field').forEach(field => {
    const index = Number(field.closest('.macro-card').dataset.index);
    field.style.opacity = controls[index].behavior === 0 ? '.45' : '1';
  });
}

function setDirty(value) {
  changed = value;
  dirtyState.textContent = value ? 'Unsaved changes' : 'All changes saved';
  dirtyState.style.color = value ? '#f4d878' : '#d6e9d9';
  saveButton.disabled = !port || !value;
}

function testControlName(index) {
  if (index < 14) return layers[0][index]?.name || `Control ${index + 1}`;
  return index === 14 ? 'Encoder clockwise' : 'Encoder counter-clockwise';
}

function logKeyTest(index, event) {
  const entry = document.createElement('li');
  entry.textContent = `${testControlName(index)}  ${event}`;
  keyTesterLog.prepend(entry);
  while (keyTesterLog.children.length > 12) keyTesterLog.lastElementChild.remove();
  const physicalKey = keymapPreview.querySelector(`[data-map-index="${index}"]`);
  physicalKey?.classList.add('key-test-hit');
  setTimeout(() => physicalKey?.classList.remove('key-test-hit'), 180);
}

function logCalibration(row, column, event) {
  if (event !== 'DOWN') return;
  const entry = document.createElement('li');
  entry.textContent = `R${row} C${column}`;
  calibrationLog.append(entry);
  calibrationStatus.textContent = `${calibrationLog.children.length} switch${calibrationLog.children.length === 1 ? '' : 'es'} recorded`;
}

function applyPaletteKey(label) {
  const quickLink = quickLinkOptions.find(([name]) => name.toLowerCase() === label.toLowerCase());
  const app = appOptions.find(([name]) => name.toLowerCase() === label.toLowerCase());
  const windowsShortcut = windowsShortcutOptions.find(([name]) => name.toLowerCase() === label.toLowerCase());
  const modifier = modifierOptions.find(([, name]) => name.toLowerCase() === label.toLowerCase());
  if (quickLink) {
    controls[selectedIndex].key = quickLink[1];
    controls[selectedIndex].modifier = 0;
  } else if (app) {
    controls[selectedIndex].key = app[1];
    controls[selectedIndex].modifier = app[2];
  } else if (windowsShortcut) {
    controls[selectedIndex].key = windowsShortcut[1];
    controls[selectedIndex].modifier = windowsShortcut[2];
  } else if (modifier) {
    controls[selectedIndex].modifier = modifier[0];
  } else if (label === 'Keymap 0' || label === 'Keymap 1') {
    activeLayer = Number(label.slice(-1));
    controls = layers[activeLayer];
    document.querySelectorAll('[data-layer]').forEach(tab => tab.classList.toggle('active', Number(tab.dataset.layer) === activeLayer));
    render();
    return;
  } else {
    const key = keyOptions.find(([, name]) => name.toLowerCase() === label.toLowerCase());
    const media = mediaOptions.find(([name]) => name.toLowerCase() === label.toLowerCase());
    const macro = macroOptions.find(([name]) => name.toLowerCase() === label.toLowerCase());
    const selected = key || (media && [media[1], media[0]]) || (macro && [macro[1], macro[0]]);
    if (!selected) return;
    controls[selectedIndex].key = selected[0];
  }
  setDirty(true);
  sendSetting(selectedIndex);
  updateInspector();
  renderKeymap();
}

function renderPalette(category = 'BASIC') {
  paletteKeys.innerHTML = paletteDefinitions[category].map(label => `<button class="palette-key">${label}</button>`).join('');
  paletteKeys.querySelectorAll('.palette-key').forEach(button => button.addEventListener('click', () => applyPaletteKey(button.textContent.trim())));
}

function keyStep(key, modifier = 0) {
  return [key, modifier, 40];
}

function textToSteps(text) {
  const punctuation = {
    '!': [30, 2], '@': [31, 2], '#': [32, 2], '$': [33, 2], '%': [34, 2],
    '^': [35, 2], '&': [36, 2], '*': [37, 2], '(': [38, 2], ')': [39, 2],
    '-': [45, 0], '_': [45, 2], '=': [46, 0], '+': [46, 2],
    '[': [47, 0], '{': [47, 2], ']': [48, 0], '}': [48, 2],
    ';': [51, 0], ':': [51, 2], "'": [52, 0], '"': [52, 2],
    ',': [54, 0], '<': [54, 2], '.': [55, 0], '>': [55, 2], '/': [56, 0], '?': [56, 2],
    ' ': [44, 0], '\n': [40, 0]
  };
  return [...text].slice(0, 8).map(character => {
    if (/[a-z]/.test(character)) return keyStep(4 + character.charCodeAt(0) - 97);
    if (/[A-Z]/.test(character)) return keyStep(4 + character.toLowerCase().charCodeAt(0) - 97, 2);
    if (/\d/.test(character)) {
      const digit = Number(character);
      return keyStep(30 + (digit === 0 ? 9 : digit - 1));
    }
    return punctuation[character] ? keyStep(...punctuation[character]) : null;
  }).filter(Boolean);
}

function comboToSteps(sequence) {
  return sequence.split(',').map(item => item.trim()).filter(Boolean).slice(0, 8).map(item => {
    const parts = item.split('+').map(part => part.trim()).filter(Boolean);
    if (parts.length > 5) return null;
    const keyName = parts.pop();
    const key = keyOptions.find(([, name]) => name.toLowerCase() === keyName.toLowerCase());
    const modifier = parts.reduce((value, part) => {
      const option = modifierOptions.find(([, name]) => name.toLowerCase() === part.toLowerCase());
      return value | (option ? option[0] : 0);
    }, 0);
    return key ? keyStep(key[0], modifier) : null;
  }).filter(Boolean);
}

function editMacro() {
  macroInput.value = '';
  macroDialog.showModal();
  macroInput.focus();
}

function sendSetting(index) {
  if (!port) return;
  const item = controls[index];
  const command = activeLayer === 1 ? 'SETL,1' : 'SET';
  send(`${command},${index},${item.key},${item.modifier},${item.behavior},${item.longKey}`);
}

function send(line) {
  if (!writer) return Promise.resolve();
  writeQueue = writeQueue.then(() => writer.write(new TextEncoder().encode(`${line}\n`)));
  return writeQueue;
}

function handleLine(line) {
  if (line.startsWith('CAL,')) {
    const [, row, column, event] = line.split(',');
    logCalibration(Number(row), Number(column), event);
    return;
  }
  if (line.startsWith('OK,CAL,')) return;
  if (line.startsWith('KEYTEST,')) {
    const [, index, event] = line.split(',');
    logKeyTest(Number(index), event);
    return;
  }
  if (line.startsWith('OK,KEYTEST,')) return;
  if (line === 'OK,SAVED') {
    setDirty(false);
    send('GET');
    send('GET,1');
    return;
  }
  if (line.startsWith('CFG,')) {
    const [, layer, index, key, modifier, behavior, longKey] = line.split(',').map(Number);
    if (layers[layer]?.[index]) Object.assign(layers[layer][index], { key, modifier, behavior, longKey });
    render();
    return;
  }
  if (line === 'END') {
    setDirty(false);
    render();
  }
}

async function readLoop() {
  const decoder = new TextDecoderStream();
  port.readable.pipeTo(decoder.writable);
  reader = decoder.readable.getReader();
  let buffer = '';
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += value;
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop();
      lines.filter(Boolean).forEach(handleLine);
    }
  } catch (error) {
    if (port) {
      port = undefined;
      writer = undefined;
      writeQueue = Promise.resolve();
      setDisconnected(error.message);
    }
  } finally {
    reader?.releaseLock();
    reader = undefined;
  }
}

function setConnected() {
  statusDot.classList.add('connected');
  statusText.textContent = 'Connected';
  connectButton.textContent = 'Disconnect';
  saveButton.disabled = !changed;
  keyTesterToggle.disabled = false;
  calibrationToggle.disabled = false;
  portName.textContent = 'ESP32-S3 live profile';
}

function setDisconnected(message = 'Disconnected') {
  statusDot.classList.remove('connected');
  statusText.textContent = message;
  connectButton.textContent = 'Connect device';
  saveButton.disabled = true;
  keyTestEnabled = false;
  calibrationEnabled = false;
  keyTesterToggle.disabled = true;
  keyTesterToggle.textContent = 'Start tester';
  calibrationToggle.disabled = true;
  calibrationToggle.textContent = 'Start calibration';
  portName.textContent = 'No device selected';
}

async function closeConnection() {
  const activePort = port;
  const activeReader = reader;
  const activeWriter = writer;
  port = undefined;
  reader = undefined;
  writer = undefined;
  writeQueue = Promise.resolve();
  try { await activeReader?.cancel(); } catch {}
  try { activeReader?.releaseLock(); } catch {}
  try { activeWriter?.releaseLock(); } catch {}
  try { await activePort?.close(); } catch {}
  setDisconnected();
}

async function connect() {
  if (!('serial' in navigator)) {
    setDisconnected('Use Chrome or Edge');
    return;
  }
  if (port) {
    await closeConnection();
    return;
  }
  try {
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    await port.open({ baudRate: 115200 });
    writer = port.writable.getWriter();
    setConnected();
    readLoop();
    send('GET');
    send('GET,1');
  } catch (error) {
    await closeConnection();
    setDisconnected(error.name === 'NotFoundError' ? 'No device selected' : 'Connection failed');
  }
}

navigator.serial?.addEventListener('disconnect', event => {
  if (event.port === port) {
    port = undefined;
    reader = undefined;
    writer = undefined;
    writeQueue = Promise.resolve();
    setDisconnected('Device disconnected');
  }
});

connectButton.addEventListener('click', connect);
saveButton.addEventListener('click', () => send('SAVE'));
selectedTest.addEventListener('click', () => send(`TEST,${selectedIndex}`));
keyTesterToggle.addEventListener('click', () => {
  keyTestEnabled = !keyTestEnabled;
  send(`KEYTEST,${keyTestEnabled ? 1 : 0}`);
  keyTesterToggle.textContent = keyTestEnabled ? 'Stop tester' : 'Start tester';
});
keyTesterClear.addEventListener('click', () => { keyTesterLog.replaceChildren(); });
calibrationToggle.addEventListener('click', () => {
  calibrationEnabled = !calibrationEnabled;
  send(`CAL,${calibrationEnabled ? 1 : 0}`);
  calibrationToggle.textContent = calibrationEnabled ? 'Stop calibration' : 'Start calibration';
  calibrationStatus.textContent = calibrationEnabled ? 'Press switches one at a time.' : 'Calibration stopped';
});
calibrationClear.addEventListener('click', () => {
  calibrationLog.replaceChildren();
  calibrationStatus.textContent = 'Press each switch in order.';
});
document.querySelectorAll('[data-layer]').forEach(button => {
  button.addEventListener('click', () => {
    activeLayer = Number(button.dataset.layer);
    controls = layers[activeLayer];
    document.querySelectorAll('[data-layer]').forEach(tab => tab.classList.toggle('active', tab === button));
    selectedIndex = 0;
    render();
  });
});
document.querySelectorAll('[data-category]').forEach(button => {
  button.addEventListener('click', () => {
    document.querySelectorAll('[data-category]').forEach(tab => tab.classList.toggle('active', tab === button));
    renderPalette(button.dataset.category);
  });
});
editMacroButton.addEventListener('click', editMacro);
document.querySelectorAll('[data-macro-mode]').forEach(button => button.addEventListener('click', () => {
  document.querySelectorAll('[data-macro-mode]').forEach(mode => mode.classList.toggle('active', mode === button));
  const comboMode = button.dataset.macroMode === 'combo';
  macroInputLabel.textContent = comboMode ? 'Key combo' : 'Text string';
  macroInput.placeholder = comboMode ? 'Example: Ctrl+Win+Alt+Shift+S' : 'Example: hello';
  macroInputHint.textContent = comboMode ? 'Use up to five keys per combo. Separate multiple combos with commas.' : 'Up to 8 characters are converted into individual key presses.';
}));
macroCancel.addEventListener('click', () => macroDialog.close());
macroSave.addEventListener('click', () => {
  const slot = controls[selectedIndex].key - 224;
  const comboMode = document.querySelector('[data-macro-mode].active').dataset.macroMode === 'combo';
  const steps = comboMode ? comboToSteps(macroInput.value) : textToSteps(macroInput.value);
  if (!steps.length) return;
  send(`MACRO,${slot},${steps.length},${steps.flat().join(',')}`);
  setDirty(true);
  macroDialog.close();
});
resetButton.addEventListener('click', () => {
  if (!confirm('Reset every control to None?')) return;
  const savedLayer = activeLayer;
  const savedControls = controls;
  layers.forEach((layer, layerIndex) => {
    layer.splice(0, layer.length, ...defaultControls.map(control => ({ ...control })));
    activeLayer = layerIndex;
    controls = layer;
    layer.forEach((_, index) => sendSetting(index));
  });
  activeLayer = savedLayer;
  controls = savedControls;
  render();
  setDirty(true);
  if (writer) send('SAVE');
});

renderPalette();
render();
