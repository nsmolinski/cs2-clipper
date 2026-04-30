"use strict";
const electron = require("electron");
electron.contextBridge.exposeInMainWorld("api", {
  closeApp: () => electron.ipcRenderer.send("app-close"),
  minimizeApp: () => electron.ipcRenderer.send("app-minimize"),
  maximizeApp: () => electron.ipcRenderer.invoke("app-maximize"),
  isMaximized: () => electron.ipcRenderer.invoke("app-is-maximized"),
  onCS2Status: (callback) => electron.ipcRenderer.on("cs2-status", (_, value) => callback(value)),
  startCapture: () => electron.ipcRenderer.invoke("start-capture"),
  stopCapture: () => electron.ipcRenderer.invoke("stop-capture"),
  saveClip: () => electron.ipcRenderer.invoke("save-clip")
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
});
