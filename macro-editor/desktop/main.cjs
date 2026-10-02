const { app, BrowserWindow, session } = require('electron');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const editorRoot = path.resolve(__dirname, '..');
let server;
let serialConfigured = false;

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8'
};

function startEditorServer() {
  return new Promise((resolve, reject) => {
    server = http.createServer((request, response) => {
      const requestedPath = request.url === '/' ? '/index.html' : request.url;
      const filePath = path.resolve(editorRoot, `.${decodeURIComponent(requestedPath)}`);
      if (!filePath.startsWith(editorRoot) || !fs.existsSync(filePath)) {
        response.writeHead(404);
        response.end('Not found');
        return;
      }

      response.writeHead(200, {
        'Content-Type': contentTypes[path.extname(filePath)] || 'application/octet-stream',
        'Cache-Control': 'no-store'
      });
      fs.createReadStream(filePath).pipe(response);
    });

    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve(server.address().port));
  });
}

function configureSerialPermissions() {
  if (serialConfigured) return;
  serialConfigured = true;
  const defaultSession = session.defaultSession;
  defaultSession.setPermissionCheckHandler((webContents, permission) => permission === 'serial');
  defaultSession.setPermissionRequestHandler((webContents, permission, callback) => {
    callback(permission === 'serial');
  });
  defaultSession.on('select-serial-port', (event, portList, webContents, callback) => {
    event.preventDefault();
    const esp32Port = portList.find(({ usbVendorId }) => usbVendorId === 0x303a);
    callback((esp32Port || portList[0])?.portId);
  });
  if (defaultSession.setDevicePermissionHandler) {
    defaultSession.setDevicePermissionHandler(({ deviceType }) => deviceType === 'serial');
  }
}

async function createWindow() {
  configureSerialPermissions();
  const port = await startEditorServer();
  const window = new BrowserWindow({
    width: 1440,
    height: 980,
    minWidth: 980,
    minHeight: 720,
    backgroundColor: '#11151b',
    icon: path.join(editorRoot, 'build', 'icon.ico'),
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false
    }
  });
  await window.loadURL(`http://127.0.0.1:${port}/`);
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (server) server.close();
  if (process.platform !== 'darwin') app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});