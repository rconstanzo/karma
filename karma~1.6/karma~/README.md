karma~ by Rodrigo Constanzo & raja & pete
==================================

The karma~ object is a dynamically lengthed varispeed sample-recording and playback object with complex functionality, which references the audio information stored in a buffer~ object having the same name.

Version 1.6 (current beta version):
The mac version (.mxo) is a 64-bit UB2, working on both Intel and Apple Silicon machines, for Max 7 or later. The windows version comes in the form of a 64-bit Intel external (.mxe64), for Max 7 (64-bit) or Max 8 or later.
!! NOTE !! The Windows versions are still version 1.1 and NOT updated with the new features - coming soon.


You can find out more information and download the public release version here:
http://www.rodrigoconstanzo.com/karma

You can follow the GitHub repository here (including older 32-bit versions):
https://github.com/rconstanzo/karma

==================================


To build karma~ on your machine, clone this git repo into the `source/audio` directory of the Max SDK and use cmake as instructed in the Max-SDK 8.2 readme files.

If you choose to place the git repo somewhere else on your system, the project won't build immediately after cloning, and you will have to futz with the build configuraiton in order to get it working.


==================================

Copyright (c) 2015-2017, Rodrigo Constanzo
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FreeBSD Project.

