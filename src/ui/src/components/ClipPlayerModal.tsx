import { useEffect, useRef, useState } from "react"

import { Clip } from "../types/clip"

import xIcon2 from "../assets/x-icon2.svg"



type ClipPlayerModalProps = {

  activeClip: Clip

  activeClipUrl: string

  onClose: () => void

}



const formatTime = (seconds: number): string => {

  if (!Number.isFinite(seconds)) return "00:00"

  const safeSeconds = Math.max(0, Math.floor(seconds))

  const mins = Math.floor(safeSeconds / 60)

  const secs = safeSeconds % 60

  return `${mins.toString().padStart(2, "0")}:${secs.toString().padStart(2, "0")}`

}



const ClipPlayerModal = ({ activeClip, activeClipUrl, onClose }: ClipPlayerModalProps) => {

  const videoRef = useRef<HTMLVideoElement | null>(null)

  const containerRef = useRef<HTMLDivElement | null>(null)

  const [isPlaying, setIsPlaying] = useState(false)

  const [currentTime, setCurrentTime] = useState(0)

  const [duration, setDuration] = useState(0)

  const [volume, setVolume] = useState(1)

  const [isMuted, setIsMuted] = useState(false)



  useEffect(() => {

    const video = videoRef.current

    if (!video) return



    setCurrentTime(0)

    setDuration(0)

    setIsPlaying(false)



    video.currentTime = 0

    video.play().catch(() => {

      setIsPlaying(false)

    })

  }, [activeClipUrl])



  const togglePlay = () => {

    const video = videoRef.current

    if (!video) return

    if (video.paused) {

      video.play().catch(() => {

        setIsPlaying(false)

      })

      return

    }

    video.pause()

  }



  const handleSeek = (value: string) => {

    const video = videoRef.current

    if (!video) return

    const nextTime = Number(value)

    video.currentTime = nextTime

    setCurrentTime(nextTime)

  }



  const handleVolume = (value: string) => {

    const video = videoRef.current

    if (!video) return

    const nextVolume = Number(value)

    video.volume = nextVolume

    video.muted = nextVolume === 0

    setVolume(nextVolume)

    setIsMuted(video.muted)

  }



  const toggleMute = () => {

    const video = videoRef.current

    if (!video) return

    video.muted = !video.muted

    setIsMuted(video.muted)

  }



  const toggleFullscreen = async () => {

    const container = containerRef.current

    if (!container) return

    if (document.fullscreenElement) {

      await document.exitFullscreen()

      return

    }

    await container.requestFullscreen()

  }



  return (

    <div

      className="fixed inset-0 z-50 bg-black/70 flex items-center justify-center p-6"

      onClick={onClose}

    >

      <div

        ref={containerRef}

        className="w-full max-w-5xl bg-[#181818] border border-[#333] rounded-xl p-4"

        onClick={(event) => event.stopPropagation()}

      >

        <div className="flex items-center justify-between mb-3">

          <p className="text-zinc-200 text-sm truncate">{activeClip.title}</p>

          <img src={xIcon2} className="w-4 h-4 cursor-pointer" onClick={onClose} />

        </div>

        <video

          ref={videoRef}

          src={activeClipUrl}

          className="w-full max-h-[75vh] rounded-lg bg-black"

          onPlay={() => setIsPlaying(true)}

          onPause={() => setIsPlaying(false)}

          onLoadedMetadata={(event) => setDuration(event.currentTarget.duration || 0)}

          onTimeUpdate={(event) => setCurrentTime(event.currentTarget.currentTime)}

          onVolumeChange={(event) => {

            setVolume(event.currentTarget.volume)

            setIsMuted(event.currentTarget.muted)

          }}

          onError={(event) => {

            const mediaError = event.currentTarget.error

            console.error("Video playback error", {

              path: activeClip.path,

              code: mediaError?.code,

              message: mediaError?.message,

            })

          }}

        />

        <div className="mt-3 space-y-3">

          <input

            type="range"

            min={0}

            max={duration || 0}

            step={0.1}

            value={Math.min(currentTime, duration || 0)}

            onChange={(event) => handleSeek(event.target.value)}

            className="w-full accent-orange-400"

          />

          <div className="flex items-center justify-between gap-3 text-sm text-zinc-200">

            <div className="flex items-center gap-2">

              <button

                onClick={togglePlay}

                className="px-3 py-1 rounded-md bg-zinc-700 hover:bg-zinc-600"

              >

                {isPlaying ? "Pause" : "Play"}

              </button>

              <button

                onClick={toggleMute}

                className="px-3 py-1 rounded-md bg-zinc-700 hover:bg-zinc-600"

              >

                {isMuted ? "Unmute" : "Mute"}

              </button>

              <input

                type="range"

                min={0}

                max={1}

                step={0.01}

                value={isMuted ? 0 : volume}

                onChange={(event) => handleVolume(event.target.value)}

                className="accent-orange-400"

              />

            </div>

            <div className="flex items-center gap-3">

              <span>{formatTime(currentTime)} / {formatTime(duration)}</span>

              <button

                onClick={() => void toggleFullscreen()}

                className="px-3 py-1 rounded-md bg-zinc-700 hover:bg-zinc-600"

              >

                Fullscreen

              </button>

            </div>

          </div>

        </div>

      </div>

    </div>

  )

}



export default ClipPlayerModal

