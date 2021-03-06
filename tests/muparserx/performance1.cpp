/*
Tests performance of muParserX as used by PAMHD.

Copyright 2014, 2015, 2016 Ilja Honkonen
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

* Neither the names of the copyright holders nor the names of their contributors
  may be used to endorse or promote products derived from this software
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "array"
#include "chrono"
#include "iostream"
#include "vector"

#include "mpParser.h"

using namespace std;
using namespace std::chrono;

int main()
{
	mup::ParserX parser(mup::pckCOMMON | mup::pckNON_COMPLEX | mup::pckMATRIX | mup::pckUNIT);
	mup::Value value1{0.0};
	mup::Variable variable1{&value1};
	parser.DefineVar("v1", variable1);
	parser.SetExpr("v1 + 1");

	const auto time_start = high_resolution_clock::now();

	for (int i = 0; i < 1000000; i++) {
		value1 = double(i);
		if (parser.Eval().GetInteger() != i + 1) {
			std::cerr <<  __FILE__ << "(" << __LINE__<< ") "
				<< "Wrong value for variable: " << parser.Eval().GetInteger()
				<< ", should be: " << i + 1
				<< endl;
			abort();
		}
	}

	const auto time_end = high_resolution_clock::now();

	const auto total = duration_cast<duration<double>>(time_end - time_start).count();
	if (total > 0.2) {
		std::cerr <<  __FILE__ << "(" << __LINE__<< ") "
			<< "muParserX performance test took too long (s): " << total
			<< endl;
		abort();
	}

	return 0;
}
