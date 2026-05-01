import './App.css'
import { useMemo, useState } from 'react'
import TitleBar from './components/TitleBar'
import RecordingsList from './components/RecordingsList'
import ClipperMenu from './components/ClipperMenu'
import { Clip } from './types/clip'
import ClipPlayerModal from './components/ClipPlayerModal'

const toFileUrl = (clipPath: string): string => {
  const normalized = clipPath.replace(/\\/g, "/")
  return `file:///${encodeURI(normalized)}`
}

function App() {
  const [activeClip, setActiveClip] = useState<Clip | null>(null)
  const activeClipUrl = useMemo(
    () => (activeClip ? toFileUrl(activeClip.path) : ""),
    [activeClip]
  )

  return (
    <>
      <TitleBar/>
      <div className="space-y-2 p-4">
        <ClipperMenu/>
        <RecordingsList onOpenClip={setActiveClip} />
      </div>
      {activeClip ? <ClipPlayerModal activeClip={activeClip} activeClipUrl={activeClipUrl} onClose={() => setActiveClip(null)} /> : null}
    </>
  )
}

export default App
