# Overview
WinTee arose out of a need for something similar to the UNIX [tee][1] command on Windows. Tee can be piped output from another command and it will pump it to both the console and a file. There is a Windows port of tee, but it obscures the exit code of the original application.

Over time, features were added to do a few other things.

This project also happens to be a reasonable example of how to process output and error streams correctly using C.

# Usage

`WinTee.exe [-pid <pid_file>] [-n[c|l][o|e]] <command and arguments>`

WinTee looks at an environment variable `LOGFILE` to determine where to send its output (in addition to sending it to the console). In this way, you can have a script that sets the variable once and then executes a whole bunch of commands.

`-pid <pid_file>`
Specifies a text file to output WinTee's process identifier (PID). This is useful for later killing the process using `TASKKILL /T /F /PID xxx`.

`-n[c|l][o|e]`
Suppresses console (c) or log (l) output (o) or error (e) streams. For example, `-nle` will suppress the error stream meant for the log file.

# Building

The solution file was created under Visual Studio 2012. There are no dependencies other than windows.h and stdio.h, so you should be able to create a project file for another version if needed.

[1]: http://en.wikipedia.org/wiki/Tee_%28command%29