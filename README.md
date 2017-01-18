# nBUS

This is an old project of mine started back in a day I was still working as a Linux/UNIX sysadmin. I needed a reliable way to store multiple credentials and other info in memory, available for other processess to acquire. Picture it as a keyring.

Authentication was meant to be based only on UNIX/Linux file permissions (i.e. the requestor UID is acquired and compared to UID of the user who saved data in the daemon in a first place; if it's a match - data will be flushed to a file ("file" in an abstract way) and kept there for predefined period of time).

It is working quite allright. The main problem I've found is that there is no reliable way to identify requestor UID on SunOS. So if root logs in, switches to some user and attempts to use this daemon it will be unable to determine whether the UID is 0 or anything else.

Making the repo public now hoping someone might make a use of either the whole project or at least parts of it :) Go ahead, fiest on it! :)

P.S. sorry for formatting... I was writing this baby back when I thought TAB indentations are cool :)
