karma~ by Rodrigo Constanzo & raja
==================================

The karma~ object is a dynamically lengthed varispeed sample-recording and playback object with complex functionality, which references the audio information stored in a buffer~ object having the same name.

For the mac version (.mxo) there are 2 versions of the external included (along with the help file). The (b) version is the 32/64bit version that will run in any version of Max (5/6/7, or 32/64). The non-b version is a purely 64bit external. The features in both are the same, it just has to do with how things are calculated internally. The windows version comes in the form of a 32bit version (.mxe) and a 64bit version (.mxe64).

You can find out more information and download the latest version here:
http://www.rodrigoconstanzo.com/karma

Or you can follow the GitHub repository here:
https://github.com/rconstanzo/karma

==================================


To build karma~ on your machine, clone this git repo into the `source` directory of the Max SDK.

For example, if your Max SDK lives in `~/Documents/Max 7/Packages/max-sdk-7.1.0`, then the path to the Xcode project for the (b) version of the external should be at `~/Documents/Max 7/Packages/max-sdk-7.1.0/source/karma/karma~(b)/karma~.xcodeproj`

If you choose to place the git repo somewhere else on your system, the project won't build immediately after cloning, and you will have to futz with the build configuraiton in order to get it working.


==================================

Copyright (c) 2015, Rodrigo Constanzo
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