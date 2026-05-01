import { useState, useEffect, useCallback } from "react"
import { Clip } from "../types/clip"

type RecordingsListProps = {
    onOpenClip: (clip: Clip) => void
}

const RecordingsList = ({ onOpenClip }: RecordingsListProps) => {
    const [clips, setClips] = useState<Clip[]>([])
    const loadClips = useCallback(() => {
        window.api.getClips().then(setClips)
    }, [])

    useEffect(() => {
        loadClips()
        const unsubscribe = window.api.onClipsUpdated(loadClips)
        return () => {
            unsubscribe()
        }
    }, [loadClips])
    
    return (
        <div className="grid grid-cols-3 gap-4">
            {clips.map((rec) => (
                <div 
                    key={rec.path}
                    className="group bg-[#1f1f1f] hover:bg-[#252525] border border-[#333] hover:border-orange-500/30 
                            rounded-xl p-4 flex gap-4 items-center transition-all duration-200 cursor-pointer"
                    onClick={() => onOpenClip(rec)}
                >
                    {rec.thumbnail ? (
                        <div className="w-28 h-20 bg-zinc-800 rounded-lg overflow-hidden flex-shrink-0">
                        <img 
                            src={rec.thumbnail} 
                            alt={rec.map || rec.title}
                            className="w-full h-full object-cover"
                        />
                    </div>
                    ) : null}
                    {!rec.thumbnail ? (
                        <div className="w-28 h-20 bg-zinc-800 rounded-lg flex-shrink-0 flex items-center justify-center text-zinc-400 text-xs">
                            Odtworz
                        </div>
                    ) : null}

                    <div className="flex-1 min-w-0">
                        <div className="flex items-center gap-2">
                            <span className="text-orange-400 font-mono text-sm">{rec.map || "Unknown map"}</span>
                            {rec.date ? <span className="text-xs text-zinc-500">•</span> : null}
                            {rec.date ? <span className="text-zinc-400 text-sm">{rec.date}</span> : null}
                        </div>
                        
                        <p className="text-white text-sm mt-1 truncate">
                            {rec.title || "Untitled Clip"}
                        </p>

                        <div className="flex gap-4 text-xs text-zinc-500 mt-2">
                            <span>{rec.duration ? `${rec.duration} seconds` : "Unknown duration"}</span>
                            {typeof rec.kills === "number" ? <span>{rec.kills} kills</span> : null}
                            {rec.type ? <span>{rec.type}</span> : null}
                        </div>
                    </div>

                </div>
            ))}
        </div>
    )
}
export default RecordingsList