/*
Box geometry class of PAMHD.

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


#ifndef PAMHD_BOUNDARIES_BOX_HPP
#define PAMHD_BOUNDARIES_BOX_HPP


#include "cstdlib"
#include "iostream"
#include "string"

#include "boost/program_options.hpp"
#include "prettyprint.hpp"

#include "program_options_validators.hpp"


namespace pamhd {
namespace boundaries {


/*!
Vector_T is assumed to be std::array or similar.
*/
template<class Vector_T> class Box
{
public:

	//! Class can read only one instance using boost::program_options
	static constexpr bool scalar = true;


	size_t get_number_of_instances() const
	{
		return 1;
	}


	/*!
	Returns true if cell spanning given volume overlaps
	this box, false otherwise.
	*/
	bool overlaps(
		const Vector_T& cell_start,
		const Vector_T& cell_end
	) {
		for (size_t i = 0; i < size_t(cell_start.size()); i++) {
			if (cell_start[i] > cell_end[i]) {
				std::cerr <<  __FILE__ << "(" << __LINE__<< ") "
					<< "Starting coordinate of cell at index " << i
					<< " is larger than ending coordinate: "
					<< cell_start[i] << " > " << cell_end[i]
					<< std::endl;
				abort();
			}
		}

		bool overlaps = true;
		for (size_t i = 0; i < size_t(cell_start.size()); i++) {
			if (
				cell_start[i] >= this->end[i]
				or cell_end[i] <= this->start[i]
			) {
				overlaps = false;
				break;
			}
		}

		return overlaps;
	}


	void add_options(
		const std::string& option_name_prefix,
		boost::program_options::options_description& options
	) {
		options.add_options()
			((option_name_prefix + "start").c_str(),
				boost::program_options::value<Vector_T>(&this->start)
					->default_value(this->start),
				"Start coordinate of box")
			((option_name_prefix + "end").c_str(),
				boost::program_options::value<Vector_T>(&this->end)
					->default_value(this->end),
				"End coordinate of box");
	}

	bool set_geometry(
		const Vector_T& given_start,
		const Vector_T& given_end
	) {
		for (size_t i = 0; i < size_t(given_start.size()); i++) {
			if (given_end[i] <= given_start[i]) {
				return false;
			}
		}

		this->start = given_start;
		this->end = given_end;
		return true;
	}

	const Vector_T& get_start() const
	{
		return this->start;
	}

	const Vector_T& get_end() const
	{
		return this->end;
	}


private:

	Vector_T start, end;
};

}} // namespaces

#endif // ifndef PAMHD_BOUNDARIES_BOX_HPP
