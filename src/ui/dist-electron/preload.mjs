"use strict";
const electron = require("electron");
electron.contextBridge.exposeInMainWorld("api", {
  closeApp: () => electron.ipcRenderer.send("app-close"),
  minimizeApp: () => electron.ipcRenderer.send("app-minimize"),
  maximizeApp: () => electron.ipcRenderer.invoke("app-maximize"),
  isMaximized: () => electron.ipcRenderer.invoke("app-is-maximized")
});
electron.contextBridge.exposeInMainWorld("ipcRenderer", {
  on(...args) {
    const [channel, listener] = args;
    return electron.ipcRenderer.on(channel, (event, ...args2) => listener(event, ...args2));
  },
  off(...args) {
    const [channel, ...omit] = args;
    return electron.ipcRenderer.off(channel, ...omit);
  },
  send(...args) {
    const [channel, ...omit] = args;
    return electron.ipcRenderer.send(channel, ...omit);
  },
  invoke(...args) {
    const [channel, ...omit] = args;
    return electron.ipcRenderer.invoke(channel, ...omit);
  }
  // You can expose other APTs you need here.
  // ...
});
