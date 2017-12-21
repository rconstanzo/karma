karma~ by Rodrigo Constanzo & raja & pete
==================================

The karma~ object is a dynamically lengthed varispeed sample-recording and playback object with complex functionality, which references the audio information stored in a buffer~ object having the same name.

Version 1.1 (current public release):
For the mac version (.mxo) there are 2 versions of the external included (along with the help file). The (b) version is the 32/64bit version that will run in any version of Max (versions 5 or 6 or 7, 32 or 64 bit). The non-b version is a purely 64-bit external. The features in both are the same. The windows version comes in the form of a 32-bit version (.mxe) and a 64-bit version (.mxe64).

Version 1.4 (interim experimental build, do not use):
OS X only. This experimental version adds feature requests and so, but was never intended for full public release. Here for comparison only.

Version 1.5 (current development version, fully functional):
OS X only for now, 32 & 64 bit UB (.mxo). Like version 1.4 - feature additions and bug fixes. Up to 4 channels operation, optional audio rate sync outlet (object arg #3 flag), state-machine added to data (list) outlet, etc.

Version 2.0 (future version):
Soon.

You can find out more information and download the public release version here:
http://www.rodrigoconstanzo.com/karma

You can follow the GitHub repository here:
https://github.com/rconstanzo/karma

==================================


To build karma~ on your machine, clone this git repo into the `source` directory of the Max SDK.

For example, if your Max SDK lives in `~/Documents/Max 7/Packages/max-sdk-7.1.0`, then the path to the Xcode project for the 1.1 (b) version of the external should be at `~/Documents/Max 7/Packages/max-sdk-7.1.0/source/karma/karma~1.1/karma~(b)/karma~.xcodeproj`

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

