import './App.css'
import TitleBar from './components/TitleBar'
import RecordingsList from './components/RecordingsList'
import ClipperMenu from './components/ClipperMenu'
function App() {

  return (
    <>
      <TitleBar/>
      <div className="space-y-2 p-4">
        <ClipperMenu/>
        <RecordingsList/>
      </div>
    </>
  )
}

export default App
