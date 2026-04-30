import { ipcRenderer, contextBridge } from 'electron'

contextBridge.exposeInMainWorld('api', {
  closeApp: () => ipcRenderer.send('app-close'),
  minimizeApp: () => ipcRenderer.send('app-minimize'),
  maximizeApp : () => ipcRenderer.invoke('app-maximize'),
  isMaximized : () => ipcRenderer.invoke('app-is-maximized'),
  onCS2Status: (callback: (state: boolean) => void) => ipcRenderer.on("cs2-status", (_, value) => callback(value)),
  startCapture: () => ipcRenderer.invoke("start-capture"),
  stopCapture: () => ipcRenderer.invoke("stop-capture"),
  saveClip: () => ipcRenderer.invoke("save-clip")
})

contextBridge.exposeInMainWorld('ipcRenderer', {
  on(...args: Parameters<typeof ipcRenderer.on>) {
    const [channel, listener] = args
    return ipcRenderer.on(channel, (event, ...args) => listener(event, ...args))
  },
  off(...args: Parameters<typeof ipcRenderer.off>) {
    const [channel, ...omit] = args
    return ipcRenderer.off(channel, ...omit)
  },
  send(...args: Parameters<typeof ipcRenderer.send>) {
    const [channel, ...omit] = args
    return ipcRenderer.send(channel, ...omit)
  },
  invoke(...args: Parameters<typeof ipcRenderer.invoke>) {
    const [channel, ...omit] = args
    return ipcRenderer.invoke(channel, ...omit)
  },

})
