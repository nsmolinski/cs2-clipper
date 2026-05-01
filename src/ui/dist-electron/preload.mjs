"use strict";
const electron = require("electron");
electron.contextBridge.exposeInMainWorld("api", {
  closeApp: () => electron.ipcRenderer.send("app-close"),
  minimizeApp: () => electron.ipcRenderer.send("app-minimize"),
  maximizeApp: () => electron.ipcRenderer.invoke("app-maximize"),
  isMaximized: () => electron.ipcRenderer.invoke("app-is-maximized"),
  onCS2Status: (callback) => {
    const listener = (_event, value) => callback(value);
    electron.ipcRenderer.on("cs2-status", listener);
    return () => electron.ipcRenderer.removeListener("cs2-status", listener);
  },
  onClipsUpdated: (callback) => {
    const listener = () => callback();
    electron.ipcRenderer.on("clips-updated", listener);
    return () => electron.ipcRenderer.removeListener("clips-updated", listener);
  },
  startCapture: () => electron.ipcRenderer.invoke("start-capture"),
  stopCapture: () => electron.ipcRenderer.invoke("stop-capture"),
  saveClip: () => electron.ipcRenderer.invoke("save-clip"),
  getClips: () => electron.ipcRenderer.invoke("get-clips")
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
