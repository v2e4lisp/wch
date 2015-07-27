# watch file change

```
Usage: ./wch [-01wW] [-d dir] [-x exclude] command

Options:
  -h           Show this help message
  -1           Run the command only once even if mulitple events occured at the same time. DEFAULT
  -0           Disalbe -1
  -w           Wait for the last command to exit. DEFAULT.
  -W           Do not wait the last command.
  -d=dir       Watch dir. DEFAULT is the current directory.
  -x=paths     Files and directories to ignore. You can specify multiple paths.
```
