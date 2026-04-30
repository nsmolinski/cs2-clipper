import clipper from "../assets/cs2-clipper.png"
const recordings = [
    {"id": 1, "thumbnail": clipper, "map": "../assets/cs2-clipper.png", "date": "30.04.2026", "title": "cs2-recording-1", "duration": 30, "kills": 20, "type": "Fast"},
    {"id": 1, "thumbnail": clipper, "map": "../assets/cs2-clipper.png", "date": "30.04.2026", "title": "cs2-recording-1", "duration": 30, "kills": 20, "type": "Fast"},
    {"id": 1, "thumbnail": clipper, "map": "../assets/cs2-clipper.png", "date": "30.04.2026", "title": "cs2-recording-1", "duration": 30, "kills": 20, "type": "Fast"},
    {"id": 1, "thumbnail": clipper, "map": "../assets/cs2-clipper.png", "date": "30.04.2026", "title": "cs2-recording-1", "duration": 30, "kills": 20, "type": "Fast"},
    {"id": 1, "thumbnail": clipper, "map": "../assets/cs2-clipper.png", "date": "30.04.2026", "title": "cs2-recording-1", "duration": 30, "kills": 20, "type": "Fast"}
]
const RecordingsList = () => {
    return (
        <>
            {recordings.map((rec) => (
                <div 
                    key={rec.id}
                    className="group bg-[#1f1f1f] hover:bg-[#252525] border border-[#333] hover:border-orange-500/30 
                            rounded-xl p-4 flex gap-4 items-center transition-all duration-200 cursor-pointer"
                >
                    <div className="w-28 h-20 bg-zinc-800 rounded-lg overflow-hidden flex-shrink-0">
                        <img 
                            src={rec.thumbnail} 
                            alt={rec.map}
                            className="w-full h-full object-cover"
                        />
                    </div>

                    <div className="flex-1 min-w-0">
                        <div className="flex items-center gap-2">
                            <span className="text-orange-400 font-mono text-sm">{rec.map}</span>
                            <span className="text-xs text-zinc-500">•</span>
                            <span className="text-zinc-400 text-sm">{rec.date}</span>
                        </div>
                        
                        <p className="text-white font-medium truncate mt-1">
                            {rec.title || "Untitled Clip"}
                        </p>

                        <div className="flex gap-4 text-xs text-zinc-500 mt-2">
                            <span>{rec.duration} seconds</span>
                            <span>{rec.kills} kills</span>
                            <span>{rec.type}</span>
                        </div>
                    </div>

                    <div className="flex gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
                        <button className="p-2 hover:bg-zinc-700 rounded-lg">
                            ▶️
                        </button>
                        <button className="p-2 hover:bg-zinc-700 rounded-lg">
                            ↓
                        </button>
                    </div>
                </div>
            ))}
        </>
    )
}
export default RecordingsList