@echo off
if exist L:\ebtl\res\NUL goto copyfile
goto end
:copyfile
copy L:\ExplorerBgToolRe\Release\ExplorerBgToolRe.dll L:\ebtl\res\ExplorerBgToolRe.bin
goto end
:end
