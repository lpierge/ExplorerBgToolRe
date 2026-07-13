@echo off
if exist L:\ebtl\res\NUL goto copyfile
goto end
:copyfile
copy L:\ExplorerBgToolRe\Debug\ExplorerBgToolRe.dll L:\ebtl\res\ExplorerBgToolRe.bin
goto end
:end
