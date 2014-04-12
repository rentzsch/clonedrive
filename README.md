Usage
-----

Chances are you need to use sudo. clonedrive's first argument is the source drive, the second argument the destination drive:

`sudo ./clonedrive /dev/rdisk8 /dev/rdisk9`

Here's a sample run:

	$ sudo ./clonedrive /dev/rdisk4 /dev/rdisk5
	src drive size: 2000398934016 bytes
	dst drive size: 2000398934016 bytes
	initial read of src drive 100% (1863.016685 GB of 1863.016685 GB)
	verifying repeatable read of src drive 100% (1863.016685 GB of 1863.016685 GB)
	repeatable read of src drive successful
	cloning 100% (1863.016685 GB of 1863.016685 GB)
	verifying written data (it's now safe to unplug the src drive)
	verifying write 100% (1863.016685 GB of 1863.016685 GB)
	SUCCESS

Why?
----

I backup my MacBook Air to an external portable USB drive.

It houses two partitions: a SuperDuper partition and a Time Machine partition. Both are encrypted.

Once I week I clone this drive to another drive since Time Machine likes to eat itself once every blue moon and for offsite backup.

I only recently started encrypting my backup drives, but sadly discovered that apparently Disk Utility's Restore functionality -- what I previously used to clone my backup drive -- doesn't work with encrypted Core Storage volumes.

So I've given up on Disk Utility. Again. And finally wrote my own drive cloner.

I could have used `dd`, but I'm never sure what a good size `bs` argument is. Pick a small one and performance is garbage, pick a big one and I'm unsure what happens to that last remainder block that doesn't fit.

clonedrive does the right thing, currently 512MB at time.

It also releases the source drive after the copy but before the verification process. That allows me to put my backup drive back into service faster.

Repeatable Read Verification
----------------------------

Like many of us, clonedrive has grown paranoid with age.

clonedrive now verifies the source drive *before* overwriting the destination drive. The common case is that the destination drive already contains an outdated-but-valid backup, and it would be a Bad Thing if that valid backup was corrupted simply because the source has started to die.

clonedrive stresses drives since it reads or writes every. single. block. I/O errors that wouldn't have shown up under normal spare FS access patterns can surface with clonedrive and tools of its ilk.

So the idea is to probe the source drive for issues before it can take out a valid backup. clonedrive does this by reading the entire source drive twice before starting to overwrite the destination drive.

Reading the source drive once ensures we can actually read every block on it. Reading it twice and comparing checksums increases the odds the drive and I/O subsystem is actually capable and isn't lying to us.

Once Repeatable Read Verification passes, cloning commences. But a third checksum is produced by the clone itself, so that's checked as well. If that doesn't match, then we know the data changed while in flight. Which is Bad, and probably indicates an dying source drive or I/O subsystem issue. Or just a cosmic ray.

Easter Egg: if you don't specify the destination drive, clonedrive will only perform Repeatable Read Verification on the specified drive. Think of it as Poor Man's [Stressdrive](https://github.com/rentzsch/stressdrive).

TODO
----

- Overlap reading and writing when cloning. Should speed things up significantly. The problem is that it seems to dramatically complicate the code, which I'm not willing to do just yet. clonedrive is meant to be a refuge of simple functionality for the paranoid. There's a reason it's written in straight-up C.

Version History
---------------

### v1.2.2: Apr 12 2014

- [FIX] Exit with failure if clone verification fails. Effect is mostly cosmetic, with SUCCESS reported after FAILURE.

### v1.2.1: Apr 11 2014

- [FIX] Close src drive before Clone Verification. ([rentzsch](https://github.com/rentzsch/clonedrive/commit/27842e80a6feb9535c2f0dc8a07e025d19ee5913))

### v1.2: Apr 10 2014

- [NEW] `--no-repeatable-read` (short form: `-norr`) option to disable Repeatable Read Verification, which is take a long time. Repeatable Read Verification remains on by default.
- [NEW] Running clonedrive with no arguments now reports its version number.
- [DEV] Better structure to argument processing, removes sole use of goto.

### v1.1.1: Apr 08 2014

- [FIX] Seek to position 0 on source drive before starting cloning in case previous code left the position somewhere else. ([rentzsch](https://github.com/rentzsch/clonedrive/commit/60c2a6215bb0058286f52e8739bd5f103d33a1d4))

### v1.1: Apr 08 2014

- [NEW] Implement Repeatable Read verification.

### v1.0.1: Nov 16 2013

- [FIX] Remove progress-report debug code. ([rentzsch](https://github.com/rentzsch/clonedrive/commit/b87e53f4b11731dc6c225704fe7d546e359d6124))

### v1.0: May 23 2013

- Initial release.