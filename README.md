Usage
=====

`sudo ./clonedrive /dev/rdisk8 /dev/rdisk9`

Here's a sample run:

	$ sudo ./clonedrive /dev/rdisk4 /dev/rdisk5
	src drive size: 2000398934016 bytes
	dst drive size: 2000398934016 bytes
	cloning 100% (1863.000000 GB of 1863.016685 GB)
	d927a232ea8ff79a1fe62a6cde9035aa53cb528b  /dev/rdisk4
	verifying written data (it's now safe to unplug the src drive)
	verifying 100% (1863.000000 GB of 1863.016685 GB)
	d927a232ea8ff79a1fe62a6cde9035aa53cb528b  /dev/rdisk5
	SUCCESS

Why?
====

I backup my MacBook Air to an external FireWire HDD Toaster.

It houses two partitions: a SuperDuper partition and a Time Machine partition. Both are encrypted.

Once I week I clone this drive to another drive since Time Machine likes to eat itself once every blue moon and for offsite backup.

I only recently started encrypting my backup drives, but sadly discovered that apparently Disk Utility's Restore functionality -- what I previously used to clone my backup drive -- doesn't work with encrypted Core Storage volumes.

So I've given up on Disk Utility. Again. And finally wrote my own drive cloner.

I could have used `dd`, but I'm never sure what a good size `bs` argument is. Pick a small one and performance is garbage, pick a big one and I'm unsure what happens to that last remainder block that doesn't fit.

`clonedrive` does the right thing, currently 512MB at time.

It also releases the source drive after the copy but before the verification process. That allows me to put my backup drive back into service in half the time that Disk Utility did.

TODO
====

- Overlap reading and writing. Should speed things up significantly.

- Read the source drive twice and verify the checksum before commencing with the clone.

	The idea is to verify a repeatable-read before overwriting the destination drive (usually carrying an outdated-but-otherwise-good backup) with a doomed original. Must include an option to disable since it will dramatically increase the time to clone a drive.