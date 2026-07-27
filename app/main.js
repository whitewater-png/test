// Electron shell. Two windows over the same page:
//
//   stage  — an ordinary window, the one you drag the camera around in
//   mascot — frameless + transparent + always-on-top, so she stands on the desktop
//
// The bridge server runs in this process, so `npm start` is the only command.
import { app, BrowserWindow, ipcMain, screen, globalShortcut, Menu } from 'electron';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { server, PORT } from '../server/bridge.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const preload = join(here, 'preload.cjs');
const origin = `http://127.0.0.1:${PORT}`;

let stageWindow = null;
let mascotWindow = null;

function createStageWindow() {
  stageWindow = new BrowserWindow({
    width: 760,
    height: 940,
    title: 'スペース長押しで話しかけられます（ドラッグで視点回転）',
    backgroundColor: '#101014',
    webPreferences: { preload, contextIsolation: true, nodeIntegration: false },
  });
  stageWindow.loadURL(`${origin}/?mode=stage`);
  stageWindow.on('closed', () => {
    stageWindow = null;
  });
  return stageWindow;
}

function createMascotWindow() {
  const { workArea } = screen.getPrimaryDisplay();
  const width = 340;
  const height = 460;

  mascotWindow = new BrowserWindow({
    width,
    height,
    x: workArea.x + 24,
    y: workArea.y + workArea.height - height - 24,
    frame: false,
    transparent: true,
    hasShadow: false,
    resizable: true,
    skipTaskbar: true,
    alwaysOnTop: true,
    fullscreenable: false,
    webPreferences: { preload, contextIsolation: true, nodeIntegration: false },
  });

  // Float above full-screen apps too, not just normal windows.
  mascotWindow.setAlwaysOnTop(true, 'floating');
  mascotWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });

  // Start click-through; the renderer raycasts the character and flips this back
  // on whenever the cursor is actually over her. `forward: true` keeps mousemove
  // flowing so that hit test can run at all.
  mascotWindow.setIgnoreMouseEvents(true, { forward: true });

  mascotWindow.loadURL(`${origin}/?mode=mascot`);
  mascotWindow.on('closed', () => {
    mascotWindow = null;
  });
  return mascotWindow;
}

ipcMain.on('mascot:set-interactive', (event, interactive) => {
  const win = BrowserWindow.fromWebContents(event.sender);
  if (!win || win !== mascotWindow) return;
  win.setIgnoreMouseEvents(!interactive, { forward: true });
});

function buildMenu() {
  Menu.setApplicationMenu(
    Menu.buildFromTemplate([
      ...(process.platform === 'darwin' ? [{ role: 'appMenu' }] : []),
      {
        label: 'ウィンドウ',
        submenu: [
          {
            label: 'ステージを開く',
            accelerator: 'CmdOrCtrl+1',
            click: () => (stageWindow ? stageWindow.focus() : createStageWindow()),
          },
          {
            label: 'デスクトップの子を表示 / 非表示',
            accelerator: 'CmdOrCtrl+Shift+M',
            click: toggleMascot,
          },
          { type: 'separator' },
          { role: 'reload' },
          { role: 'toggleDevTools' },
          { role: 'close' },
        ],
      },
    ]),
  );
}

function toggleMascot() {
  if (!mascotWindow) return createMascotWindow();
  if (mascotWindow.isVisible()) mascotWindow.hide();
  else mascotWindow.show();
}

app.whenReady().then(() => {
  server.listen(PORT, '127.0.0.1', () => {
    console.log(`[mascot] bridge listening on ${origin}`);
    createStageWindow();
    createMascotWindow();
  });

  buildMenu();
  globalShortcut.register('CommandOrControl+Shift+M', toggleMascot);

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createStageWindow();
  });
});

app.on('will-quit', () => globalShortcut.unregisterAll());

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
