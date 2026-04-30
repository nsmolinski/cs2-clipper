import Logo from '../assets/cs2-clipper.png'
import maximizeIcon from '../assets/maximize-icon.svg'
import restoreIcon from '../assets/restore-icon.svg'
import xIcon from '../assets/x-icon.svg'
import minusIcon from '../assets/minus-icon.svg'
import { useEffect, useState } from "react"
const TitleBar = () => {
    const [maximized, setMaximized] = useState(false);
    const checkMaximized = async () => {
        const result = await window.api.isMaximized();
        setMaximized(result);
    };
    useEffect(() => {
        checkMaximized();

        window.addEventListener("resize", checkMaximized);
        return () => window.removeEventListener("resize", checkMaximized);
    }, []);

    const handleMaximize = async () => {
        await window.api.maximizeApp();
        checkMaximized();
    };
    return(
        <div className="fixed z-1000 w-full title-bar h-14 flex items-center justify-between [-webkit-app-region:drag] border-b border-[#333]">
            <div className='flex items-center ml-4 gap-2'>
                <img src={Logo} className="w-8 h-8" />
                <h1 className='text-sm font-medium tracking-wider uppercase'> <span className='text-orange-400'>CS2</span> Clipper</h1>
            </div>
            <div className="flex items-center h-full">
                <div className='flex justify-center items-center w-12 h-full [-webkit-app-region:no-drag] h-full' onClick={() => window.api.minimizeApp()}>
                    <img src={minusIcon} className='w-4'/>
                </div>
                
                    {maximized ? (
                        <div className='flex justify-center items-center w-12 h-full [-webkit-app-region:no-drag] h-full' onClick={handleMaximize}>
                            <img src={restoreIcon} className="w-4"/>
                        </div>
                    ) : (
                        <div className='flex justify-center items-center w-12 h-full [-webkit-app-region:no-drag]' onClick={handleMaximize}>
                            <img src={maximizeIcon} className="w-4"/>
                        </div>
                    )}
                <div className="[-webkit-app-region:no-drag] flex justify-center w-12 hover:bg-red-700 h-full flex items-center transition-colors duration-200">
                    <img src={xIcon} className='w-4'
                        onClick={() => window.api.closeApp()}/>
                </div>

            </div>
        </div>
    )
}
export default TitleBar