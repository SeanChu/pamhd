/*
Class for handling the initial condition of a simulation in one region.

Copyright 2016 Ilja Honkonen
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

#ifndef PAMHD_BOUNDARIES_VALUE_BOUNDARY_HPP
#define PAMHD_BOUNDARIES_VALUE_BOUNDARY_HPP


#include "algorithm"
#include "array"
#include "stdexcept"
#include "string"
#include "type_traits"
#include "utility"

#include "rapidjson/document.h"
#include "mpParser.h"

#include "boundaries/common.hpp"
#include "boundaries/math_expression.hpp"


namespace pamhd {
namespace boundaries {


/*!
Collection of boundaries creatable from arguments given on command line.
*/
template<
	class Geometry_Id,
	class Variable
> class Value_Boundary
{
public:


	/*!
	Prepares value boundary from given rapidjson object.

	Otherwise similar to Initial_Condition::set() except
	that given object must have a "time_stamps" array of
	numbers and a "values" key which points to array data
	instead of "value" key which points to one value. Data
	pointed to by "data" key is also in order
	(x_1, y_1, z_1, t_1), (x_2, y_1, z_1, t_1), ...

	Example jsons which could be given as object:
	\verbatim
	{"geometry_id": 0, "time_stamps": [1], "values": [123]}
	\endverbatim
	\verbatim
	{"geometry_id": 1, "time_stamps": [-1, 2], "values": ["sin(t)", "cos(x)"]}
	\endverbatim
	\verbatim
	{
		"geometry_id": 0,
		"time_stamps": [1, 2, 3],
		"values": {
			"x": [1, 2],
			"y": [-3, 2],
			"z": [0.1, 0.2],
			"data": [1, 2, 3, ..., 24]
		}
	}
	\endverbatim
	*/
	void set(const rapidjson::Value& object)
	{
		if (not object.HasMember("geometry_id")) {
			throw std::invalid_argument(__FILE__ ": object doesn't have a geometry_id key.");
		}
		const auto& json_geometry_id = object["geometry_id"];

		if (not json_geometry_id.IsUint()) {
			throw std::invalid_argument(__FILE__ ": geometry_id is not unsigned int.");
		}
		this->geometry_id = json_geometry_id.GetUint();


		if (not object.HasMember("time_stamps")) {
			throw std::invalid_argument(__FILE__ ": object doesn't have a time_stamps key.");
		}
		const auto& json_time_stamps = object["time_stamps"];

		if (not json_time_stamps.IsArray()) {
			throw std::invalid_argument(__FILE__ ": time_stamps is not an array.");
		}

		this->time_stamps.clear();
		this->time_stamps.reserve(json_time_stamps.Size());
		for (size_t i = 0; i < json_time_stamps.Size(); i++) {
			this->time_stamps.push_back(json_time_stamps[i].GetDouble());
		}

		if (not std::is_sorted(this->time_stamps.cbegin(), this->time_stamps.cend())) {
			throw std::invalid_argument(__FILE__ ": time stamps aren't sorted in non-descending order.");
		}


		if (not object.HasMember("values")) {
			throw std::invalid_argument(__FILE__ ": object doesn't have a values key.");
		}
		const auto& values = object["values"];

		try {
			fill_variable_value_from_json(
				values,
				this->value_bdy_type,
				this->number_values,
				this->math_expressions,
				this->coordinates,
				this->coordinate_type,
				this->data
			);
		} catch (const std::invalid_argument& error) {
			throw std::invalid_argument(
				std::string(__FILE__ "(") + std::to_string(__LINE__) + ") "
				+ " Couldn't set variable from json object: "
				+ error.what()
			);
		}

		// check that json object had correct amount of data
		size_t correct_data_size = this->time_stamps.size();
		switch (this->value_bdy_type) {
		case 1:
			if (this->number_values.size() != correct_data_size) {
				throw std::invalid_argument(
					std::string(__FILE__ "(") + std::to_string(__LINE__) + ") "
					+ " Lengths of time stamps and values don't match: "
					+ std::to_string(correct_data_size) + " vs "
					+ std::to_string(this->number_values.size())
				);
			}
			break;
		case 2:
			if (this->math_expressions.size() != correct_data_size) {
				throw std::invalid_argument(
					std::string(__FILE__ "(") + std::to_string(__LINE__) + ") "
					+ " Lengths of time stamps and math expressions don't match: "
					+ std::to_string(correct_data_size) + " vs "
					+ std::to_string(this->math_expressions.size())
				);
			}
			break;
		case 3:
			correct_data_size
				*= this->coordinates[0].size()
				* this->coordinates[1].size()
				* this->coordinates[2].size();
			if (this->data.size() != correct_data_size) {
				throw std::invalid_argument(
					std::string(__FILE__ "(") + std::to_string(__LINE__) + ") "
					+ " Lengths of time stamps * coordinates and data don't match: "
					+ std::to_string(correct_data_size) + " vs "
					+ std::to_string(this->data.size())
				);
			}
			break;
		default:
			throw std::out_of_range(
					std::string(__FILE__ "(") + std::to_string(__LINE__) + ") "
					+ " Invalid this->value_bdy_type: "
					+ std::to_string(this->value_bdy_type)
				);
		}
	}


	typename Variable::data_type get_data(
		const double& t,
		const double& x,
		const double& y,
		const double& z,
		const double& radius,
		const double& latitude,
		const double& longitude
	) {
		const auto after = std::lower_bound(
			this->time_stamps.cbegin(),
			this->time_stamps.cend(),
			t
		);

		size_t time_index;
		if (after == this->time_stamps.cbegin()) {
			time_index = 0;
		} else if (after == this->time_stamps.cend()) {
			time_index = this->time_stamps.size() - 1;
		} else {
			const auto before = after - 1;
			if (std::abs(t - *before) <= std::abs(t - *after)) {
				time_index = std::distance(this->time_stamps.cbegin(), before);
			} else {
				time_index = std::distance(this->time_stamps.cbegin(), after);
			}
		}

		switch (this->value_bdy_type) {
		case 1:
			return this->number_values[time_index];

		case 2:
			return this->math_expressions[time_index].evaluate(
				t, x, y, z, radius, latitude, longitude
			);

		// nearest neighbor interpolation in point cloud
		case 3:
			return this->data[
				find_data(
					[&]()
						-> std::array<double, 3>
					{
						switch (this->coordinate_type) {
						case 1:
							return {x, y, z};
						case 2:
							return {radius, latitude, longitude};
						default:
							throw std::out_of_range(__FILE__ ": Invalid coordinate type.");
						}
					}(),
					this->coordinates
				)
				// take into account current time index
				+ time_index
					* this->coordinates[0].size()
					* this->coordinates[1].size()
					* this->coordinates[2].size()
			];

		default:
			throw std::out_of_range(__FILE__ ": Invalid initial condition type.");
		}
	}


	Geometry_Id get_geometry_id() const
	{
		return this->geometry_id;
	}

	void set_geometry_id(const Geometry_Id& id)
	{
		this->geometry_id = id;
	}


private:

	// region where this initial condition is applied
	Geometry_Id geometry_id;

	// value boundary if array of numbers was given in json file
	std::vector<typename Variable::data_type> number_values;

	// value boundary if array of strings was given in json file
	std::vector<Math_Expression<Variable>> math_expressions;

	// value boundary if object was given in json file
	std::array<std::vector<double>, 3> coordinates; // e.g. x,y,z coordinates of points
	int coordinate_type = -1; // cartesian == 1, geographic == 2

	std::vector<double> time_stamps;

	// data in order x[0],y[0],z[0],t[0]; x[1],y[0],z[0],t[0]; ...
	std::vector<typename Variable::data_type> data;

	int value_bdy_type = -1; // number == 1, string == 2, object == 3
};

}} // namespaces

#endif // ifndef PAMHD_BOUNDARIES_VALUE_BOUNDARY_HPP