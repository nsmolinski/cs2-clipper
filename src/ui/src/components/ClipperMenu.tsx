import { useState, useEffect } from "react"

const ClipperMenu = () => {
    const [isGameLaunched, setGameLaunched] = useState(false)
    const [isTracking, setIsTracking] = useState(false)
    useEffect(() => {
        window.api.onCS2Status(setGameLaunched)
    }, [])
    const handleCapture = async () => {
        setIsTracking(true)
        await window.api.startCapture()
    }
    return (
        <div className="mt-12 py-4">
            {isGameLaunched && !isTracking ? (
                <button className="text-green-500 inline-block cursor-pointer border-2 border-green-500 p-2 px-4 rounded-md [text-shadow:0_0_0.25px_#22c55e,0_0_10px_#22c55e] [box-shadow:0_0_0.25px_#22c55e,0_0_10px_#22c55e]" onClick={handleCapture}>Start capturing</button>
            ) : (
                <>
                    {isTracking ? (
                        <button className="text-green-500 inline-block cursor-pointer border-2 border-green-500 p-2 px-4 rounded-md [text-shadow:0_0_0.25px_#22c55e,0_0_10px_#22c55e] [box-shadow:0_0_0.25px_#22c55e,0_0_10px_#22c55e]">Alt+F12 to save clip</button>
                    ) : (
                        <button className="text-red-500 inline-block cursor-pointer border-2 border-red-500 p-2 px-4 rounded-md [text-shadow:0_0_5px_#ef4444,0_0_10px_#ef4444] [box-shadow:0_0_5px_#ef4444,0_0_10px_#ef4444]">CS2 not launched</button>
                    )}
                </>
            )}
        </div>
    )
}
export default ClipperMenu