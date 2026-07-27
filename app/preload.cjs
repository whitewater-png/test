// The only thing the renderer needs from the main process: telling the
// transparent mascot window whether the cursor is currently over the character,
// so clicks pass through to the desktop everywhere else.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('mascot', {
  setInteractive(interactive) {
    ipcRenderer.send('mascot:set-interactive', Boolean(interactive));
  },
});
