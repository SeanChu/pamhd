/*
Particle-assisted magnetohydrodynamics.

Copyright 2015 Ilja Honkonen
All rights reserved.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.


Combination of ../particle/test.cpp and ../mhd/two_test.cpp where
particles represent one of the fluids.
*/

#include "array"
#include "cmath"
#include "cstdlib"
#include "fstream"
#include "iostream"
#include "random"
#include "string"

#include "boost/filesystem.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/numeric/odeint.hpp"
#include "boost/program_options.hpp"
#include "dccrg.hpp"
#include "dccrg_cartesian_geometry.hpp"
#include "Eigen/Core" // must be included before gensimcell
#include "Eigen/Geometry"
#include "mpi.h" // must be included before gensimcell
#include "gensimcell.hpp"

#include "boundaries/copy_boundary.hpp"
#include "boundaries/initial_condition.hpp"
#include "boundaries/value_boundaries.hpp"
#include "divergence/remove.hpp"
#include "grid_options.hpp"
#include "mhd/common.hpp"
#include "mhd/save.hpp"
#include "mhd/solve.hpp"
#include "mhd/hll_athena.hpp"
#include "mhd/hlld_athena.hpp"
#include "mhd/roe_athena.hpp"
#include "mhd/variables.hpp"
#include "particle/accumulate_dccrg.hpp"
#include "particle/common.hpp"
#include "particle/initialize.hpp"
#include "particle/save.hpp"
#include "particle/solve_dccrg.hpp"
#include "particle/solve_fields.hpp"
#include "particle/variables.hpp"
#include "pamhd/initialize.hpp"


using namespace std;

/*
See comments in ../mhd/test.cpp and ../particle/massless.cpp
for explanation of items identical to ones in those files
*/

unsigned long long int next_particle_id;

int Poisson_Cell::transfer_switch = Poisson_Cell::INIT;

struct Number_Density {
	using data_type = double;
	static std::string get_name() { return std::string("Bulk number density"); }
	static std::string get_option_name() { return std::string("bulk-number-density"); }
	static std::string get_option_help() { return std::string(""); }
};
struct Temperature {
	using data_type = double;
	static std::string get_name() { return std::string("Bulk temperature"); }
	static std::string get_option_name() { return std::string("bulk-temperature"); }
	static std::string get_option_help() { return std::string(""); }
};
struct Nr_Particles_In_Cell {
	using data_type = double;
	static std::string get_name() { return std::string("Nr particles in cell"); }
	static std::string get_option_name() { return std::string("nr-particles-in-cell"); }
	static std::string get_option_help() { return std::string(""); }
};
struct Replace_Particles {
	using data_type = double;
	static std::string get_name() { return std::string("Replace particles"); }
	static std::string get_option_name() { return std::string("replace-particles"); }
	static std::string get_option_help() { return std::string("Whether to replace (> 0) or add (<= 0) particles with create_particles()"); }
};


// cell class used by this program
using Cell = gensimcell::Cell<
	gensimcell::Optional_Transfer,
	pamhd::mhd::MHD_State_Conservative,
	pamhd::mhd::HD1_State, // fluid
	pamhd::mhd::HD2_State, // particles
	pamhd::mhd::Cell_Type,
	pamhd::mhd::MPI_Rank,
	pamhd::mhd::Magnetic_Field_Temp,
	pamhd::mhd::Magnetic_Field_Divergence,
	pamhd::mhd::Scalar_Potential_Gradient,
	pamhd::mhd::Electric_Current_Density,
	pamhd::mhd::MHD_Flux_Conservative,
	pamhd::mhd::HD1_Flux,
	pamhd::mhd::HD2_Flux,
	pamhd::particle::Electric_Field,
	pamhd::particle::Magnetic_Field,
	pamhd::particle::Bulk_Mass,
	pamhd::particle::Bulk_Momentum,
	pamhd::particle::Bulk_Velocity,
	pamhd::particle::Nr_Particles_Internal,
	pamhd::particle::Nr_Particles_External,
	pamhd::particle::Nr_Accumulated_To_Cells,
	pamhd::particle::Particles_Internal,
	pamhd::particle::Particles_External,
	pamhd::particle::Accumulated_To_Cells
>;
using Grid = dccrg::Dccrg<Cell, dccrg::Cartesian_Geometry>;


// returns a reference to internal particle list of given cell
const auto Particle_List_Getter
	= [](Cell& cell)->typename pamhd::particle::Particles_Internal::data_type&{
		return cell[pamhd::particle::Particles_Internal()];
	};

// reference to position of given particle
const auto Particle_Position_Getter
	= [](pamhd::particle::Particle_Internal& particle)
		->typename pamhd::particle::Position::data_type&
	{
		return particle[pamhd::particle::Position()];
	};

// mass of given particle
const auto Particle_Mass_Getter
	= [](pamhd::particle::Particle_Internal& particle)
		->typename pamhd::particle::Mass::data_type&
	{
		return particle[pamhd::particle::Mass()];
	};

const auto Particle_Momentum_Getter
	= [](pamhd::particle::Particle_Internal& particle)
		->typename pamhd::particle::Velocity::data_type
	{
		return particle[pamhd::particle::Mass()] * particle[pamhd::particle::Velocity()];
	};

// accumulated mass of particles in given cell
const auto Bulk_Mass_Getter
	= [](Cell& cell_data)->typename pamhd::particle::Bulk_Mass::data_type&{
		return cell_data[pamhd::particle::Bulk_Mass()];
	};

const auto Bulk_Momentum_Getter
	= [](Cell& cell_data)->typename pamhd::particle::Bulk_Momentum::data_type&{
		return cell_data[pamhd::particle::Bulk_Momentum()];
	};

// accumulated momentum / accumulated velocity of particles in given cell
const auto Bulk_Velocity_Getter
	= [](Cell& cell_data)->typename pamhd::particle::Bulk_Velocity::data_type&{
		return cell_data[pamhd::particle::Bulk_Velocity()];
	};

// list of items (variables above) accumulated from particles in given cell
const auto Accu_List_Getter
	= [](Cell& cell_data)->typename pamhd::particle::Accumulated_To_Cells::data_type&{
		return cell_data[pamhd::particle::Accumulated_To_Cells()];
	};

// length of above list (for transferring between processes)
const auto Accu_List_Length_Getter
	= [](Cell& cell_data)->typename pamhd::particle::Nr_Accumulated_To_Cells::data_type&{
		return cell_data[pamhd::particle::Nr_Accumulated_To_Cells()];
	};

// target cell of accumulated particle values in an accumulation list item
const auto Accu_List_Target_Getter
	= [](pamhd::particle::Accumulated_To_Cell& accu_item)
		->typename pamhd::particle::Target::data_type&
	{
		return accu_item[pamhd::particle::Target()];
	};

// accumulated mass of particles in an accumulation list item
const auto Accu_List_Bulk_Mass_Getter
	= [](pamhd::particle::Accumulated_To_Cell& accu_item)
		->typename pamhd::particle::Bulk_Mass::data_type&
	{
		return accu_item[pamhd::particle::Bulk_Mass()];
	};

// accumulated momentum of particles in an accumulation list item
const auto Accu_List_Bulk_Momentum_Getter
	= [](pamhd::particle::Accumulated_To_Cell& accu_item)
		->typename pamhd::particle::Bulk_Momentum::data_type&
	{
		return accu_item[pamhd::particle::Bulk_Momentum()];
	};

// reference to total mass density of all fluids in given cell
const auto Mas
	= [](Cell& cell_data)->typename pamhd::mhd::Mass_Density::data_type&{
		return cell_data[pamhd::mhd::MHD_State_Conservative()][pamhd::mhd::Mass_Density()];
	};
const auto Mom
	= [](Cell& cell_data)->typename pamhd::mhd::Momentum_Density::data_type&{
		return cell_data[pamhd::mhd::MHD_State_Conservative()][pamhd::mhd::Momentum_Density()];
	};
const auto Nrj
	= [](Cell& cell_data)->typename pamhd::mhd::Total_Energy_Density::data_type&{
		return cell_data[pamhd::mhd::MHD_State_Conservative()][pamhd::mhd::Total_Energy_Density()];
	};
const auto Mag
	= [](Cell& cell_data)->typename pamhd::mhd::Magnetic_Field::data_type&{
		return cell_data[pamhd::mhd::MHD_State_Conservative()][pamhd::mhd::Magnetic_Field()];
	};

// field before divergence removal in case removal fails
const auto Mag_tmp
	= [](Cell& cell_data)->typename pamhd::mhd::Magnetic_Field_Temp::data_type&{
		return cell_data[pamhd::mhd::Magnetic_Field_Temp()];
	};
// divergence of magnetic field
const auto Mag_div
	= [](Cell& cell_data)->typename pamhd::mhd::Magnetic_Field_Divergence::data_type&{
		return cell_data[pamhd::mhd::Magnetic_Field_Divergence()];
	};
// magnetic field for propagating particles
const auto Mag_part
	= [](Cell& cell_data)->typename pamhd::mhd::Magnetic_Field::data_type&{
		return cell_data[pamhd::particle::Magnetic_Field()];
	};
// curl of magnetic field
const auto Cur
	= [](Cell& cell_data)->typename pamhd::mhd::Electric_Current_Density::data_type&{
		return cell_data[pamhd::mhd::Electric_Current_Density()];
	};
// electric field for propagating particles
const auto Ele
	= [](Cell& cell_data)->typename pamhd::particle::Electric_Field::data_type&{
		return cell_data[pamhd::particle::Electric_Field()];
	};

// flux / total change of magnetic field over one time step
const auto Mag_f
	= [](Cell& cell_data)->typename pamhd::mhd::Magnetic_Field::data_type&{
		return cell_data[pamhd::mhd::MHD_Flux_Conservative()][pamhd::mhd::Magnetic_Field()];
	};

// reference to mass density of fluid 1 in given cell
const auto Mas1
	= [](Cell& cell_data)->typename pamhd::mhd::Mass_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_State()][pamhd::mhd::Mass_Density()];
	};
const auto Mom1
	= [](Cell& cell_data)->typename pamhd::mhd::Momentum_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_State()][pamhd::mhd::Momentum_Density()];
	};
const auto Nrj1
	= [](Cell& cell_data)->typename pamhd::mhd::Total_Energy_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_State()][pamhd::mhd::Total_Energy_Density()];
	};
// reference to mass density of fluid 2 in given cell
const auto Mas2
	= [](Cell& cell_data)->typename pamhd::mhd::Mass_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_State()][pamhd::mhd::Mass_Density()];
	};
const auto Mom2
	= [](Cell& cell_data)->typename pamhd::mhd::Momentum_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_State()][pamhd::mhd::Momentum_Density()];
	};
const auto Nrj2
	= [](Cell& cell_data)->typename pamhd::mhd::Total_Energy_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_State()][pamhd::mhd::Total_Energy_Density()];
	};

// flux of mass density of fluid 1 over one time step
const auto Mas1_f
	= [](Cell& cell_data)->typename pamhd::mhd::Mass_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_Flux()][pamhd::mhd::Mass_Density()];
	};
const auto Mom1_f
	= [](Cell& cell_data)->typename pamhd::mhd::Momentum_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_Flux()][pamhd::mhd::Momentum_Density()];
	};
const auto Nrj1_f
	= [](Cell& cell_data)->typename pamhd::mhd::Total_Energy_Density::data_type&{
		return cell_data[pamhd::mhd::HD1_Flux()][pamhd::mhd::Total_Energy_Density()];
	};
// flux of mass density of fluid 2 over one time step
const auto Mas2_f
	= [](Cell& cell_data)->typename pamhd::mhd::Mass_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_Flux()][pamhd::mhd::Mass_Density()];
	};
const auto Mom2_f
	= [](Cell& cell_data)->typename pamhd::mhd::Momentum_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_Flux()][pamhd::mhd::Momentum_Density()];
	};
const auto Nrj2_f
	= [](Cell& cell_data)->typename pamhd::mhd::Total_Energy_Density::data_type&{
		return cell_data[pamhd::mhd::HD2_Flux()][pamhd::mhd::Total_Energy_Density()];
	};


int main(int argc, char* argv[])
{
	using std::min;

	/*
	Initialize MPI
	*/

	if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
		std::cerr << "Couldn't initialize MPI." << std::endl;
		abort();
	}

	MPI_Comm comm = MPI_COMM_WORLD;

	int rank = 0, comm_size = 0;
	if (MPI_Comm_rank(comm, &rank) != MPI_SUCCESS) {
		std::cerr << "Couldn't obtain MPI rank." << std::endl;
		abort();
	}
	if (MPI_Comm_size(comm, &comm_size) != MPI_SUCCESS) {
		std::cerr << "Couldn't obtain size of MPI communicator." << std::endl;
		abort();
	}

	next_particle_id = 1 + rank;

	// intialize Zoltan
	float zoltan_version;
	if (Zoltan_Initialize(argc, argv, &zoltan_version) != ZOLTAN_OK) {
		std::cerr << "Zoltan_Initialize failed." << std::endl;
		abort();
	}


	pamhd::boundaries::Initial_Condition<
		uint64_t,
		double,
		std::array<double, 3>,
		pamhd::mhd::Magnetic_Field
	> initial_field;

	pamhd::boundaries::Value_Boundaries<
		uint64_t,
		double,
		double,
		std::array<double, 3>,
		pamhd::mhd::Magnetic_Field
	> field_value_bdy;

	pamhd::boundaries::Copy_Boundary<
		uint64_t,
		double,
		std::array<double, 3>
	> field_copy_bdy;

	pamhd::boundaries::Initial_Condition<
		uint64_t,
		double,
		std::array<double, 3>,
		pamhd::mhd::Number_Density,
		pamhd::mhd::Velocity,
		pamhd::mhd::Pressure
	> init_cond_fluid1;

	std::vector<
		// one initial condition for each population
		pamhd::boundaries::Initial_Condition<
			uint64_t,
			double,
			std::array<double, 3>,
			Number_Density,
			Temperature,
			pamhd::particle::Bulk_Velocity,
			Nr_Particles_In_Cell,
			pamhd::particle::Charge_Mass_Ratio,
			pamhd::particle::Species_Mass,
			Replace_Particles
		>
	> initial_particles;

	pamhd::boundaries::Value_Boundaries<
		uint64_t,
		double,
		double,
		std::array<double, 3>,
		pamhd::mhd::Number_Density,
		pamhd::mhd::Velocity,
		pamhd::mhd::Pressure
	> value_bdy_fluid1;

	pamhd::boundaries::Value_Boundaries<
		uint64_t,
		double,
		double,
		std::array<double, 3>,
		Number_Density,
		Temperature,
		pamhd::particle::Bulk_Velocity,
		Nr_Particles_In_Cell,
		pamhd::particle::Charge_Mass_Ratio,
		pamhd::particle::Species_Mass
	> particles_value_bdy;

	pamhd::grid::Options grid_options;
	grid_options.data.set_expression(pamhd::grid::Number_Of_Cells(), "{1, 1, 1}");
	grid_options.data.set_expression(pamhd::grid::Volume(), "{1, 1, 1}");
	grid_options.data.set_expression(pamhd::grid::Start(), "{0, 0, 0}");
	grid_options.data.set_expression(pamhd::grid::Periodic(), "{false, false, false}");

	bool verbose = false;
	size_t
		poisson_iterations_max = 1000,
		poisson_iterations_min = 0,
		nr_initial_populations = 1;
	double
		max_time_step = std::numeric_limits<double>::infinity(),
		save_particle_n = -1,
		start_time = 0,
		end_time = 1,
		time_step_factor = 0.5,
		remove_div_B_n = 0.1,
		poisson_norm_stop = 1e-10,
		poisson_norm_increase_max = 10,
		adiabatic_index = 5.0 / 3.0,    
		vacuum_permeability = 4e-7 * M_PI,
		proton_mass = 1.672621777e-27,
		particle_temp_nrj_ratio = 1.3806488e-23;

	std::string
		mhd_solver_str("hll_athena"),
		config_file_name(""),
		lb_name("RCB"),
		output_directory("./");

	boost::program_options::options_description
		general_options("Usage: program_name [options], where options are"),
		initial_field_options(""),
		initial_particle_options(""),
		initial_fluid_options(""),
		value_boundary_options(""),
		copy_boundary_options("");

	// handle general options
	general_options.add_options()
		("help", "Print this help message")
		("verbose", "Print run time information")
		("initial-help", "Print help for initial condition options")
		("config-file",
			boost::program_options::value<std::string>(&config_file_name)
				->default_value(config_file_name),
			"Read options also from file arg (command line has priority, "
			"not read if empty string)")
		("output-directory",
			boost::program_options::value<std::string>(&output_directory)
				->default_value(output_directory),
			"Output simulation results into directory arg (relative to "
			"current working directory, must include final dir separator")
		("time-start",
			boost::program_options::value<double>(&start_time)
				->default_value(start_time),
			"Start time of simulation (s)")
		("time-length",
			boost::program_options::value<double>(&end_time)
				->default_value(end_time),
			"Length of simulation (s)")
		("save-particle-n",
			boost::program_options::value<double>(&save_particle_n)
				->default_value(save_particle_n),
			"Save results every arg seconds, 0 saves "
			"initial and final states, -1 doesn't save")
		("nr-initial-populations",
			boost::program_options::value<size_t>(&nr_initial_populations)
				->default_value(nr_initial_populations),
			"Number of initial particle populations")
		("time-step-factor",
			boost::program_options::value<double>(&time_step_factor)
				->default_value(time_step_factor),
			"Multiply maximum allowed time step (CFL condition) with factor arg")
		("max-time-step",
			boost::program_options::value<double>(&max_time_step)
				->default_value(max_time_step),
			"Maximum length of simulation time step, before " "physical/mathematical/numerical considerations")
		("vacuum-permeability",
			boost::program_options::value<double>(&vacuum_permeability)
				->default_value(vacuum_permeability),
			"https://en.wikipedia.org/wiki/Vacuum_permeability in V*s/(A*m)")
		("adiabatic-index",
			boost::program_options::value<double>(&adiabatic_index)
				->default_value(adiabatic_index),
			"https://en.wikipedia.org/wiki/Heat_capacity_ratio")
		("proton-mass",
			boost::program_options::value<double>(&proton_mass)
				->default_value(proton_mass),
			"Mass of a proton in kg")
		("load-balancer",
			boost::program_options::value<std::string>(&lb_name)
				->default_value(lb_name),
			"Load balancing algorithm to use (for example RANDOM, "
			"RCB, HSFC, HYPERGRAPH; for a list of available algorithms see "
			"http://www.cs.sandia.gov/zoltan/ug_html/ug_alg.html)")
		("solver-mhd",
			boost::program_options::value<std::string>(&mhd_solver_str)
				->default_value(mhd_solver_str),
			"MHD solver to use, one of: hll_athena")
		("remove-div-B-n",
			boost::program_options::value<double>(&remove_div_B_n)
				->default_value(remove_div_B_n),
			"Remove divergence of magnetic field every arg seconds (<= 0 doesn't remove)")
		("boltzmann",
			boost::program_options::value<double>(&particle_temp_nrj_ratio)
				->default_value(particle_temp_nrj_ratio),
			"Ratio of energy and temperature when calculating particle velocities (J/K, Boltzmann constant)");


	grid_options.add_options("grid.", general_options);
	initial_field.add_initialization_options("initial-field.", general_options);
	init_cond_fluid1.add_initialization_options("initial-fluid1.", general_options);
	field_value_bdy.add_initialization_options("field-value-boundaries.", general_options);
	value_bdy_fluid1.add_initialization_options("value-boundary-fluid1.", general_options);
	field_copy_bdy.add_initialization_options("field-copy-boundaries.", general_options);
	particles_value_bdy.add_initialization_options("particle-value-boundaries.", general_options);

	boost::program_options::variables_map option_variables;
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(general_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options: " << e.what()
				<< std::endl;
		}
		abort();
	}
	boost::program_options::notify(option_variables);

	if (option_variables.count("help") > 0) {
		if (rank == 0) {
			cout << general_options << endl;
		}
		MPI_Finalize();
		return EXIT_SUCCESS;
	}

	if (option_variables.count("verbose") > 0) {
		verbose = true;
	}

	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					general_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse general options from file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
		boost::program_options::notify(option_variables);
	}

	if (option_variables.count("help") > 0) {
		if (rank == 0) {
			cout << general_options << endl;
		}
		MPI_Finalize();
		return EXIT_SUCCESS;
	}

	initial_field.add_options("initial-field.", initial_field_options);
	field_value_bdy.add_options("field-value-boundaries.", value_boundary_options);
	field_copy_bdy.add_options("field-copy-boundaries.", copy_boundary_options);
	particles_value_bdy.add_options("particle-value-boundaries.", value_boundary_options);
	init_cond_fluid1.add_options("initial-fluid1.", initial_fluid_options);
	value_bdy_fluid1.add_options("value-boundary-fluid1.", value_boundary_options);

	// field initial condition
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(initial_field_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "initial condition of magnetic field: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					initial_field_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for initial condition "
					<< "of magnetic field from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);

	// fluid initial condition
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(initial_fluid_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "initial condition of magnetic field: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					initial_fluid_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for initial condition "
					<< "of magnetic field from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);

	// add particle init options
	initial_particles.resize(nr_initial_populations);
	for (size_t i = 0; i < initial_particles.size(); i++) {
		initial_particles[i].add_initialization_options(
			"initial-particles.population" + boost::lexical_cast<std::string>(i + 1) + ".",
			initial_particle_options
		);
	}
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(initial_particle_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "initial condition of particles: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					initial_particle_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for initial condition "
					<< "of particles from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);

	for (size_t i = 0; i < initial_particles.size(); i++) {
		initial_particles[i].add_options(
			"initial-particles.population" + boost::lexical_cast<std::string>(i + 1) + ".",
			initial_particle_options
		);
	}

	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(initial_particle_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "initial condition of particles: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					initial_particle_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for initial condition "
					<< "of particles from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);

	// value boundary options
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(value_boundary_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "value boundary conditions: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					value_boundary_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for value boundaries "
					<< "from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);

	// copy boundary options
	try {
		boost::program_options::store(
			boost::program_options::command_line_parser(argc, argv)
				.options(copy_boundary_options)
				.allow_unregistered()
				.run(),
			option_variables
		);
	} catch (std::exception& e) {
		if (rank == 0) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
				<< "Couldn't parse command line options for "
				<< "copy boundary conditions: "
				<< e.what()
				<< std::endl;
		}
		abort();
	}
	if (config_file_name != "") {
		try {
			boost::program_options::store(
				boost::program_options::parse_config_file<char>(
					config_file_name.c_str(),
					copy_boundary_options,
					true
				),
				option_variables
			);
		} catch (std::exception& e) {
			if (rank == 0) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Couldn't parse options for copy boundaries "
					<< "from configuration file "
					<< config_file_name << ": " << e.what()
					<< std::endl;
			}
			abort();
		}
	}
	boost::program_options::notify(option_variables);



	if (rank == 0) {
		boost::filesystem::create_directories(output_directory);
	}

	Grid grid;

	/*
	Set/update derived grid parameters based on given options
	*/

	// muparserx doesn't support uint64_t so convert from int
	const auto nr_cells_tmp
		= grid_options.data.get_data(pamhd::grid::Number_Of_Cells());

	const std::array<uint64_t, 3> number_of_cells{{
		uint64_t(nr_cells_tmp[0]),
		uint64_t(nr_cells_tmp[1]),
		uint64_t(nr_cells_tmp[2])
	}};

	const std::array<double, 3>
		simulation_volume
			= grid_options.data.get_data(pamhd::grid::Volume()),
		cell_volume{{
			simulation_volume[0] / number_of_cells[0],
			simulation_volume[1] / number_of_cells[1],
			simulation_volume[2] / number_of_cells[2]
		}};

	const unsigned int neighborhood_size = 1;
	const auto periodic = grid_options.data.get_data(pamhd::grid::Periodic());
	if (
		not grid.initialize(
			number_of_cells,
			comm,
			lb_name.c_str(),
			neighborhood_size,
			0,
			periodic[0],
			periodic[1],
			periodic[2]
		)
	) {
		std::cerr << __FILE__ << ":" << __LINE__
			<< ": Couldn't initialize grid."
			<< std::endl;
		abort();
	}

	dccrg::Cartesian_Geometry::Parameters geom_params;
	geom_params.start = grid_options.data.get_data(pamhd::grid::Start());
	geom_params.level_0_cell_length = cell_volume;

	if (not grid.set_geometry(geom_params)) {
		std::cerr << __FILE__ << "(" << __LINE__ << "): "
			<< "Couldn't set grid geometry."
			<< std::endl;
		abort();
	}

	grid.balance_load();

	// create copies of remote neighbor cells' data
	grid.update_copies_of_remote_neighbors();

	const auto
		inner_cell_ids = grid.get_local_cells_not_on_process_boundary(),
		outer_cell_ids = grid.get_local_cells_on_process_boundary(),
		remote_cell_ids = grid.get_remote_cells_on_process_boundary(),
		cell_ids = grid.get_cells();
	auto
		mhd_inner_cell_ids = inner_cell_ids,
		mhd_outer_cell_ids = outer_cell_ids;

	/*
	Initialize
	*/

	if (verbose and rank == 0) {
		cout << "Initializing magnetic field... " << endl;
	}
	pamhd::initialize_field(
		initial_field,
		grid,
		cell_ids,
		0,
		Mag
	);
	if (verbose and rank == 0) {
		cout << "done." << endl;
	}

	// set initial condition
	std::mt19937_64 random_source;

	if (verbose and rank == 0) {
		cout << "Initializing particles... " << endl;
	}
	for (size_t i = 0; i < initial_particles.size(); i++) {
		bool replace = false;
		if (
			initial_particles[i].default_data.get_data(
				Replace_Particles(), {{0, 0, 0}}, 0
			) > 0
		) {
			replace = true;
		}
		const auto nr_particles_created
			= pamhd::particle::initialize<
				Number_Density,
				Temperature,
				pamhd::particle::Charge_Mass_Ratio,
				pamhd::particle::Bulk_Velocity,
				Nr_Particles_In_Cell,
				pamhd::particle::Particles_Internal,
				pamhd::particle::Particle_Internal,
				pamhd::particle::Mass,
				pamhd::particle::Charge_Mass_Ratio,
				pamhd::particle::Position,
				pamhd::particle::Velocity,
				pamhd::particle::Particle_ID,
				pamhd::particle::Species_Mass
			>(
				initial_particles[i],
				start_time,
				cell_ids,
				grid,
				random_source,
				particle_temp_nrj_ratio,
				next_particle_id,
				grid.get_comm_size(),
				replace,
				verbose
			);
		next_particle_id += nr_particles_created * grid.get_comm_size();
	}
	if (verbose and rank == 0) {
		cout << "done." << endl;
	}

	/*
	Accumulate particle mass to Mas2 for fluid total energy adjustment
	*/
	for (const auto& cell: cell_ids) {
		auto* const cell_data = grid[cell];
		if (cell_data == nullptr) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
			abort();
		}
		// accumulate below doesn't zero the variable
		Bulk_Mass_Getter(*cell_data) = 0;
	}
	pamhd::particle::accumulate(
		cell_ids,
		grid,
		Particle_List_Getter,
		Particle_Position_Getter,
		Particle_Mass_Getter,
		Bulk_Mass_Getter,
		Accu_List_Bulk_Mass_Getter,
		Accu_List_Target_Getter,
		Accu_List_Length_Getter,
		Accu_List_Getter
	);

	Cell::set_transfer_all(true, pamhd::particle::Nr_Accumulated_To_Cells());
	grid.update_copies_of_remote_neighbors();
	Cell::set_transfer_all(false, pamhd::particle::Nr_Accumulated_To_Cells());

	allocate_accumulation_lists(
		grid,
		Accu_List_Getter,
		Accu_List_Length_Getter
	);

	Cell::set_transfer_all(true, pamhd::particle::Accumulated_To_Cells());
	grid.update_copies_of_remote_neighbors();
	Cell::set_transfer_all(false, pamhd::particle::Accumulated_To_Cells());

	pamhd::particle::accumulate_from_remote_neighbors<
		pamhd::particle::Target,
		pamhd::particle::Bulk_Mass
	>(
		grid,
		Bulk_Mass_Getter,
		Accu_List_Getter
	);
	for (const auto& cell: cell_ids) {
		auto* const cell_data = grid[cell];
		if (cell_data == nullptr) {
			std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
			abort();
		}
		const auto length = grid.geometry.get_length(cell);
		const auto volume = length[0] * length[1] * length[2];
		Mas2(*cell_data) = Bulk_Mass_Getter(*cell_data) / volume;
	}

	pamhd::initialize_fluid(
		init_cond_fluid1,
		grid, cell_ids, 0,
		adiabatic_index, vacuum_permeability, proton_mass,
		Mas1, Mom1, Nrj1
	);
	// add magnetic field contribution to total energy density
	for (const auto& cell_id: cell_ids) {
		auto* const cell_data = grid[cell_id];
		if (cell_data == nullptr) {
			std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
			abort();
		}
		const auto
			total_mass = Mas1(*cell_data) + Mas2(*cell_data),
			mass_frac1 = Mas1(*cell_data) / total_mass;
		Nrj1(*cell_data) += mass_frac1 * 0.5 * Mag(*cell_data).squaredNorm() / vacuum_permeability;
	}


	const auto mhd_solver
		= [&mhd_solver_str](){
			if (mhd_solver_str == "hll_athena") {

				return pamhd::mhd::athena::get_flux_hll<
					pamhd::mhd::MHD_State_Conservative::data_type,
					pamhd::mhd::Mass_Density,
					pamhd::mhd::Momentum_Density,
					pamhd::mhd::Total_Energy_Density,
					pamhd::mhd::Magnetic_Field
				>;

			} else {

				std::cerr <<  __FILE__ << "(" << __LINE__ << ") Invalid solver: "
					<< mhd_solver_str << ", use --help to list available solvers"
					<< std::endl;
				abort();
			}
		}();


	double
		max_dt = 0,
		simulation_time = start_time,
		next_particle_save = simulation_time + save_particle_n,
		next_rem_div_B = simulation_time + remove_div_B_n;

	size_t simulated_steps = 0;
	while (simulation_time < end_time) {
		simulated_steps++;

		double
			// don't step over the final simulation time
			until_end = end_time - simulation_time,
			local_time_step = min(min(time_step_factor * max_dt, until_end), max_time_step),
			time_step = -1;

		if (
			MPI_Allreduce(
				&local_time_step,
				&time_step,
				1,
				MPI_DOUBLE,
				MPI_MIN,
				comm
			) != MPI_SUCCESS
		) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< ": Couldn't reduce time step."
				<< std::endl;
			abort();
		}

		/*
		Calculate/update bulk particle quantities and E between processes
		*/

		// B required for E calculation
		Cell::set_transfer_all(true, pamhd::mhd::MHD_State_Conservative());
		grid.start_remote_neighbor_copy_updates();


		// inner: J for E = (J - V) x B
		pamhd::divergence::get_curl(
			inner_cell_ids,
			grid,
			Mag,
			Cur
		);
		// not included in get_curl above
		for (const auto& cell: inner_cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}
			Cur(*cell_data) /= vacuum_permeability;
		}

		// local: use MHD B for propagating particles
		for (const auto& cell: cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}
			Mag_part(*cell_data) = Mag(*cell_data);
		}

		// prepare to accumulate particle data
		for (const auto& cell: cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}
			Bulk_Mass_Getter(*cell_data) = 0;
			Bulk_Momentum_Getter(*cell_data) = {0, 0, 0};
		}
		pamhd::particle::accumulate(
			outer_cell_ids,
			grid,
			Particle_List_Getter,
			Particle_Position_Getter,
			Particle_Mass_Getter,
			Bulk_Mass_Getter,
			Accu_List_Bulk_Mass_Getter,
			Accu_List_Target_Getter,
			Accu_List_Length_Getter,
			Accu_List_Getter
		);
		pamhd::particle::accumulate(
			outer_cell_ids,
			grid,
			Particle_List_Getter,
			Particle_Position_Getter,
			Particle_Momentum_Getter,
			Bulk_Momentum_Getter,
			Accu_List_Bulk_Momentum_Getter,
			Accu_List_Target_Getter,
			Accu_List_Length_Getter,
			Accu_List_Getter,
			false // don't erase accumulated mass
		);


		grid.wait_remote_neighbor_copy_update_receives();

		// outer: J for E = (J - V) x B
		pamhd::divergence::get_curl(
			outer_cell_ids,
			grid,
			Mag,
			Cur
		);
		for (const auto& cell: outer_cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}
			Cur(*cell_data) /= vacuum_permeability;
		}

		// remote B also required for propagating local particles
		for (const auto& cell: remote_cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}
			Mag_part(*cell_data) = Mag(*cell_data);
		}

		grid.wait_remote_neighbor_copy_update_sends();
		Cell::set_transfer_all(false, pamhd::mhd::MHD_State_Conservative());

		// update lengths of accumulation lists from above
		Cell::set_transfer_all(true, pamhd::particle::Nr_Accumulated_To_Cells());
		grid.start_remote_neighbor_copy_updates();

		pamhd::particle::accumulate(
			inner_cell_ids,
			grid,
			Particle_List_Getter,
			Particle_Position_Getter,
			Particle_Mass_Getter,
			Bulk_Mass_Getter,
			Accu_List_Bulk_Mass_Getter,
			Accu_List_Target_Getter,
			Accu_List_Length_Getter,
			Accu_List_Getter
		);
		pamhd::particle::accumulate(
			inner_cell_ids,
			grid,
			Particle_List_Getter,
			Particle_Position_Getter,
			Particle_Momentum_Getter,
			Bulk_Momentum_Getter,
			Accu_List_Bulk_Momentum_Getter,
			Accu_List_Target_Getter,
			Accu_List_Length_Getter,
			Accu_List_Getter
		);

		pamhd::particle::fill_mhd_fluid_values<
			pamhd::particle::Mass,
			pamhd::particle::Velocity,
			pamhd::particle::Species_Mass
		>(
			inner_cell_ids,
			grid,
			adiabatic_index,
			vacuum_permeability,
			particle_temp_nrj_ratio,
			Bulk_Mass_Getter,
			Bulk_Momentum_Getter,
			Particle_List_Getter,
			Mas2, Mom2, Nrj2, Mag,
			true
		);

		grid.wait_remote_neighbor_copy_update_receives();

		allocate_accumulation_lists(
			grid,
			Accu_List_Getter,
			Accu_List_Length_Getter
		);

		grid.wait_remote_neighbor_copy_update_sends();
		Cell::set_transfer_all(false, pamhd::particle::Nr_Accumulated_To_Cells());


		Cell::set_transfer_all(true, pamhd::particle::Accumulated_To_Cells());
		grid.start_remote_neighbor_copy_updates();

		// inner: E = (J - V) x B
		for (const auto& cell: inner_cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}

			Bulk_Velocity_Getter(*cell_data)
				= Bulk_Momentum_Getter(*cell_data)
				/ Bulk_Mass_Getter(*cell_data);

			Ele(*cell_data)
				= (
					Cur(*cell_data)
					- Bulk_Velocity_Getter(*cell_data)
				).cross(Mag(*cell_data));
		}

		grid.wait_remote_neighbor_copy_update_receives();

		pamhd::particle::accumulate_from_remote_neighbors<
			pamhd::particle::Target,
			pamhd::particle::Bulk_Mass
		>(
			grid,
			Bulk_Mass_Getter,
			Accu_List_Getter
		);
		pamhd::particle::accumulate_from_remote_neighbors<
			pamhd::particle::Target,
			pamhd::particle::Bulk_Momentum
		>(
			grid,
			Bulk_Momentum_Getter,
			Accu_List_Getter
		);

		pamhd::particle::fill_mhd_fluid_values<
			pamhd::particle::Mass,
			pamhd::particle::Velocity,
			pamhd::particle::Species_Mass
		>(
			outer_cell_ids,
			grid,
			adiabatic_index,
			vacuum_permeability,
			particle_temp_nrj_ratio,
			Bulk_Mass_Getter,
			Bulk_Momentum_Getter,
			Particle_List_Getter,
			Mas2, Mom2, Nrj2, Mag,
			true
		);

		// calculate total values for MHD solver
		for (const auto& cell_id: outer_cell_ids) {
			auto* const cell_data = grid[cell_id];
			if (cell_data == nullptr) {
				std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
				abort();
			}
			Mas(*cell_data) = Mas1(*cell_data) + Mas2(*cell_data);
			Mom(*cell_data) = Mom1(*cell_data) + Mom2(*cell_data);
			Nrj(*cell_data) = Nrj1(*cell_data) + Nrj2(*cell_data);
		}

		// outer: E = (J - V) x B
		for (const auto& cell: outer_cell_ids) {
			auto* const cell_data = grid[cell];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << ")" << std::endl;
				abort();
			}

			Bulk_Velocity_Getter(*cell_data)
				= Bulk_Momentum_Getter(*cell_data)
				/ Bulk_Mass_Getter(*cell_data);

			Ele(*cell_data)
				= (
					Cur(*cell_data)
					- Bulk_Velocity_Getter(*cell_data)
				).cross(Mag(*cell_data));
		}

		grid.wait_remote_neighbor_copy_update_sends();
		Cell::set_transfer_all(false, pamhd::particle::Accumulated_To_Cells());


		Cell::set_transfer_all(true, pamhd::particle::Electric_Field());
		grid.update_copies_of_remote_neighbors();
		Cell::set_transfer_all(false, pamhd::particle::Electric_Field());


		/*
		Solve
		*/

		if (verbose && grid.get_rank() == 0) {
			cout << "Solving at time " << simulation_time
				<< " s with time step " << time_step << " s" << endl;
		}

		max_dt = std::numeric_limits<double>::max();

		// outer particles
		max_dt = min(
			max_dt,
			pamhd::particle::solve<
				pamhd::particle::Electric_Field,
				pamhd::particle::Magnetic_Field,
				pamhd::particle::Nr_Particles_External,
				pamhd::particle::Particles_Internal,
				pamhd::particle::Particles_External,
				pamhd::particle::Position,
				pamhd::particle::Velocity,
				pamhd::particle::Charge_Mass_Ratio,
				pamhd::particle::Mass,
				pamhd::particle::Destination_Cell,
				boost::numeric::odeint::runge_kutta_fehlberg78<pamhd::particle::state_t>
			>(time_step, outer_cell_ids, grid)
		);


		Cell::set_transfer_all(
			true,
			pamhd::mhd::MHD_State_Conservative(),
			pamhd::mhd::HD1_State(),
			pamhd::mhd::HD2_State(),
			pamhd::mhd::Cell_Type(),
			pamhd::particle::Nr_Particles_External()
		);
		grid.start_remote_neighbor_copy_updates();

		for (const auto& cell_id: inner_cell_ids) {
			auto* const cell_data = grid[cell_id];
			if (cell_data == nullptr) {
				std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
				abort();
			}
			Mas(*cell_data) = Mas1(*cell_data) + Mas2(*cell_data);
			Mom(*cell_data) = Mom1(*cell_data) + Mom2(*cell_data);
			Nrj(*cell_data) = Nrj1(*cell_data) + Nrj2(*cell_data);
		}

		// inner MHD
		max_dt = min(
			max_dt,
			pamhd::mhd::N_solve(
				mhd_solver,
				mhd_inner_cell_ids,
				grid,
				time_step,
				adiabatic_index,
				vacuum_permeability,
				Mas1, Mas, Mom, Nrj, Mag,
				Mas1_f, Mom1_f, Nrj1_f, Mag_f
			)
		);
		// needed for full flux of magnetic field
		max_dt = min(
			max_dt,
			pamhd::mhd::N_solve(
				mhd_solver,
				mhd_inner_cell_ids,
				grid,
				time_step,
				adiabatic_index,
				vacuum_permeability,
				Mas2, Mas, Mom, Nrj, Mag,
				Mas2_f, Mom2_f, Nrj2_f, Mag_f
			)
		);

		// inner particles
		max_dt = min(
			max_dt,
			pamhd::particle::solve<
				pamhd::particle::Electric_Field,
				pamhd::particle::Magnetic_Field,
				pamhd::particle::Nr_Particles_External,
				pamhd::particle::Particles_Internal,
				pamhd::particle::Particles_External,
				pamhd::particle::Position,
				pamhd::particle::Velocity,
				pamhd::particle::Charge_Mass_Ratio,
				pamhd::particle::Mass,
				pamhd::particle::Destination_Cell,
				boost::numeric::odeint::runge_kutta_fehlberg78<pamhd::particle::state_t>
			>(time_step, inner_cell_ids, grid)
		);

		grid.wait_remote_neighbor_copy_update_receives();

		// outer MHD
		max_dt = min(
			max_dt,
			pamhd::mhd::N_solve(
				mhd_solver,
				mhd_outer_cell_ids,
				grid,
				time_step,
				adiabatic_index,
				vacuum_permeability,
				Mas1, Mas, Mom, Nrj, Mag,
				Mas1_f, Mom1_f, Nrj1_f, Mag_f
			)
		);
		max_dt = min(
			max_dt,
			pamhd::mhd::N_solve(
				mhd_solver,
				mhd_outer_cell_ids,
				grid,
				time_step,
				adiabatic_index,
				vacuum_permeability,
				Mas2, Mas, Mom, Nrj, Mag,
				Mas2_f, Mom2_f, Nrj2_f, Mag_f
			)
		);

		pamhd::mhd::apply_fluxes(
			mhd_inner_cell_ids,
			grid,
			adiabatic_index,
			vacuum_permeability,
			Mas1, Mom1, Nrj1, Mag,
			Mas1_f, Mom1_f, Nrj1_f, Mag_f,
			false
		);

		pamhd::particle::resize_receiving_containers<
			pamhd::particle::Nr_Particles_External,
			pamhd::particle::Particles_External
		>(remote_cell_ids, grid);

		grid.wait_remote_neighbor_copy_update_sends();
		Cell::set_transfer_all(
			false,
			pamhd::mhd::MHD_State_Conservative(),
			pamhd::mhd::HD1_State(),
			pamhd::mhd::HD2_State(),
			pamhd::mhd::Cell_Type(),
			pamhd::particle::Nr_Particles_External()
		);


		Cell::set_transfer_all(
			true,
			pamhd::particle::Particles_External()
		);
		grid.start_remote_neighbor_copy_updates();

		pamhd::mhd::apply_fluxes(
			mhd_outer_cell_ids,
			grid,
			adiabatic_index,
			vacuum_permeability,
			Mas1, Mom1, Nrj1, Mag,
			Mas1_f, Mom1_f, Nrj1_f, Mag_f,
			false
		);

		pamhd::particle::incorporate_external_particles<
			pamhd::particle::Nr_Particles_External,
			pamhd::particle::Particles_Internal,
			pamhd::particle::Particles_External,
			pamhd::particle::Destination_Cell
		>(inner_cell_ids, grid);

		grid.wait_remote_neighbor_copy_update_receives();

		pamhd::particle::incorporate_external_particles<
			pamhd::particle::Nr_Particles_External,
			pamhd::particle::Particles_Internal,
			pamhd::particle::Particles_External,
			pamhd::particle::Destination_Cell
		>(outer_cell_ids, grid);

		pamhd::particle::remove_external_particles<
			pamhd::particle::Nr_Particles_External,
			pamhd::particle::Particles_External
		>(inner_cell_ids, grid);

		grid.wait_remote_neighbor_copy_update_sends();
		Cell::set_transfer_all(
			false,
			pamhd::particle::Particles_External()
		);

		pamhd::particle::remove_external_particles<
			pamhd::particle::Nr_Particles_External,
			pamhd::particle::Particles_External
		>(outer_cell_ids, grid);

		/*
		Solution(s) done for this step.
		*/

		// remove divergence of magnetic field
		if (remove_div_B_n > 0 and simulation_time >= next_rem_div_B) {
			next_rem_div_B += remove_div_B_n;

			if (verbose and rank == 0) {
				cout << "Removing divergence of B at time "
					<< simulation_time << "...  ";
				cout.flush();
			}

			// save old value of B in case div removal fails
			for (const auto& cell: cell_ids) {
				auto* const cell_data = grid[cell];
				if (cell_data == nullptr) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						"No data for cell " << cell
						<< std::endl;
					abort();
				}

				Mag_tmp(*cell_data) = Mag(*cell_data);
			}

			Cell::set_transfer_all(
				true,
				pamhd::mhd::MHD_State_Conservative(),
				pamhd::mhd::Magnetic_Field_Divergence()
			);
			const auto div_before
				= pamhd::divergence::remove(
					cell_ids,
					{},
					{},
					grid,
					Mag,
					Mag_div,
					[](Cell& cell_data)
						-> typename pamhd::mhd::Scalar_Potential_Gradient::data_type&
					{
						return cell_data[pamhd::mhd::Scalar_Potential_Gradient()];
					},
					poisson_iterations_max,
					poisson_iterations_min,
					poisson_norm_stop,
					2,
					poisson_norm_increase_max,
					false
				);
			Cell::set_transfer_all(false, pamhd::mhd::Magnetic_Field_Divergence());

			grid.update_copies_of_remote_neighbors();
			Cell::set_transfer_all(false, pamhd::mhd::MHD_State_Conservative());
			const double div_after
				= pamhd::divergence::get_divergence(
					cell_ids,
					grid,
					Mag,
					Mag_div
				);

			// restore old B
			if (div_after > div_before) {
				if (verbose and rank == 0) {
					cout << "failed (" << div_after
						<< "), restoring previous value (" << div_before << ")."
						<< endl;
				}
				for (const auto& cell: cell_ids) {
					auto* const cell_data = grid[cell];
					if (cell_data == nullptr) {
						std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
							"No data for cell " << cell
							<< std::endl;
						abort();
					}

					Mag(*cell_data) = Mag_tmp(*cell_data);
				}

			} else {
				if (verbose and rank == 0) {
					cout << div_before << " -> " << div_after << endl;
				}
				// TODO: keep pressure/temperature constant over div removal
				// needs latest accumulated particle mass
			}

		}

		simulation_time += time_step;


		/*
		Value boundaries
		*/
		field_value_bdy.clear_cells();
		particles_value_bdy.clear_cells();
		value_bdy_fluid1.clear_cells();

		for (const auto cell_id: cell_ids) {
			auto* const cell_data = grid[cell_id];
			if (cell_data == NULL) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"No data for cell: " << cell_id
					<< std::endl;
				abort();
			}
			(*cell_data)[pamhd::mhd::Cell_Type()] = 0;

			const auto
				cell_start = grid.geometry.get_min(cell_id),
				cell_end = grid.geometry.get_max(cell_id);

			boost::optional<size_t> result = particles_value_bdy.add_cell(
				simulation_time,
				cell_id,
				cell_start,
				cell_end
			);
			if (not result) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't add cell " << cell_id << " to particle value boundaries."
					<< std::endl;
				abort();
			}

			result = field_value_bdy.add_cell(
				simulation_time,
				cell_id,
				cell_start,
				cell_end
			);
			if (not result) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't add cell " << cell_id << " to MHD value boundary."
					<< std::endl;
				abort();
			}
			if (*result > 0) {
				(*cell_data)[pamhd::mhd::Cell_Type()] = 1;
			}

			result = value_bdy_fluid1.add_cell(
				simulation_time,
				cell_id,
				cell_start,
				cell_end
			);
			if (not result) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't add cell " << cell_id << " to value_boundaries."
					<< std::endl;
				abort();
			}
			if (*result > 0) {
				(*cell_data)[pamhd::mhd::Cell_Type()] = 1;
			}
		}

		const pamhd::mhd::Number_Density N{};
		const pamhd::mhd::Velocity V{};
		const pamhd::mhd::Pressure P{};
		const pamhd::mhd::Magnetic_Field B{};

		for (size_t bdy_id = 0; bdy_id < value_bdy_fluid1.get_number_of_boundaries(); bdy_id++) {

			for (const auto& cell_id: value_bdy_fluid1.get_cells(bdy_id)) {
				const auto cell_center = grid.geometry.get_center(cell_id);

				auto* const cell_data = grid[cell_id];
				if (cell_data == NULL) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						"No data for cell: " << cell_id
						<< std::endl;
					abort();
				}

				const auto mass_density
					= value_bdy_fluid1.get_data(N, bdy_id, cell_center, simulation_time)
					* proton_mass;
				const auto velocity
					= value_bdy_fluid1.get_data(V, bdy_id, cell_center, simulation_time);
				const auto pressure
					= value_bdy_fluid1.get_data(P, bdy_id, cell_center, simulation_time);

				// TODO: accumulate new density from particles
				Mas1(*cell_data) = mass_density;
				Mas(*cell_data) = Mas1(*cell_data) + Mas2(*cell_data);
				Mom1(*cell_data) = mass_density * velocity;
				if (mass_density > 0 and pressure > 0) {
					Nrj1(*cell_data) = pamhd::mhd::get_total_energy_density(
						mass_density,
						velocity,
						pressure,
						std::array<double, 3>{{0, 0, 0}},
						adiabatic_index,
						vacuum_permeability
					);
				} else {
					Nrj1(*cell_data) = 0;
				}
			}
		}

		for (size_t bdy_id = 0; bdy_id < field_value_bdy.get_number_of_boundaries(); bdy_id++) {
			constexpr pamhd::mhd::Magnetic_Field Mag{};

			for (const auto& cell_id: field_value_bdy.get_cells(bdy_id)) {
				const auto cell_center = grid.geometry.get_center(cell_id);

				auto* const cell_data = grid[cell_id];
				if (cell_data == NULL) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						"No data for cell: " << cell_id
						<< std::endl;
					abort();
				}

				(*cell_data)[pamhd::mhd::MHD_State_Conservative()][Mag]
					= field_value_bdy.get_data(Mag, bdy_id, cell_center, simulation_time);
			}
		}

		for (size_t bdy_id = 0; bdy_id < particles_value_bdy.get_number_of_boundaries(); bdy_id++) {

			for (const auto& cell_id: particles_value_bdy.get_cells(bdy_id)) {
				const auto
					cell_start = grid.geometry.get_min(cell_id),
					cell_end = grid.geometry.get_max(cell_id),
					cell_length = grid.geometry.get_length(cell_id),
					cell_center = grid.geometry.get_center(cell_id);

				const auto
					number_density
						= particles_value_bdy.get_data(
							Number_Density(), bdy_id, cell_center, simulation_time
						),
					temperature
						= particles_value_bdy.get_data(
							Temperature(), bdy_id, cell_center, simulation_time
						),
					charge_mass_ratio
						= particles_value_bdy.get_data(
							pamhd::particle::Charge_Mass_Ratio(), bdy_id, cell_center, simulation_time
						),
					species_mass
						= particles_value_bdy.get_data(
							pamhd::particle::Species_Mass(), bdy_id, cell_center, simulation_time
						);
				const auto bulk_velocity
					= particles_value_bdy.get_data(
						pamhd::particle::Bulk_Velocity(), bdy_id, cell_center, simulation_time
					);
				const auto nr_particles
					= particles_value_bdy.get_data(
						Nr_Particles_In_Cell(), bdy_id, cell_center, simulation_time
					);

				auto* const cell_data = grid[cell_id];
				if (cell_data == NULL) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						"No data for cell: " << cell_id
						<< std::endl;
					abort();
				}

				(*cell_data)[pamhd::particle::Particles_Internal()]
					= pamhd::particle::create_particles<
						pamhd::particle::Particle_Internal,
						pamhd::particle::Mass,
						pamhd::particle::Charge_Mass_Ratio,
						pamhd::particle::Position,
						pamhd::particle::Velocity,
						pamhd::particle::Particle_ID,
						pamhd::particle::Species_Mass
					>(
						bulk_velocity,
						Eigen::Vector3d{cell_start[0], cell_start[1], cell_start[2]},
						Eigen::Vector3d{cell_end[0], cell_end[1], cell_end[2]},
						Eigen::Vector3d{temperature, temperature, temperature},
						nr_particles,
						charge_mass_ratio,
						species_mass * number_density * cell_length[0] * cell_length[1] * cell_length[2],
						species_mass,
						particle_temp_nrj_ratio,
						random_source,
						next_particle_id,
						grid.get_comm_size()
					);
				next_particle_id += nr_particles * grid.get_comm_size();
			}
		}
		/*
		Add magnetic nrj to fluid boundary cells
		*/
		for (size_t bdy_id = 0; bdy_id < value_bdy_fluid1.get_number_of_boundaries(); bdy_id++) {
			for (const auto& cell_id: value_bdy_fluid1.get_cells(bdy_id)) {
				auto* const cell_data = grid[cell_id];
				if (cell_data == NULL) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						"No data for cell: " << cell_id
						<< std::endl;
					abort();
				}
				const auto mass_frac = Mas1(*cell_data) / Mas(*cell_data);
				Nrj1(*cell_data) += mass_frac * 0.5 * Mag(*cell_data).squaredNorm() / vacuum_permeability;
			}
		}

		/*
		Copy boundaries
		*/
		field_copy_bdy.clear_cells();

		// don't copy from other MHD boundary cells
		for (size_t bdy_id = 0; bdy_id < field_value_bdy.get_number_of_boundaries(); bdy_id++) {
			for (const auto& cell_id: field_value_bdy.get_cells(bdy_id)) {
				field_copy_bdy.add_as_other_boundary(cell_id);
			}
		}

		// classify cells
		for (const auto cell_id: cell_ids) {
			const auto
				cell_start = grid.geometry.get_min(cell_id),
				cell_end = grid.geometry.get_max(cell_id);

			auto* const cell_data = grid[cell_id];
			if (cell_data == nullptr) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"No data for cell: " << cell_id
					<< std::endl;
				abort();
			}

			if (field_copy_bdy.add_cell(cell_id, cell_start, cell_end) > 0) {
				(*cell_data)[pamhd::mhd::Cell_Type()] = 2;
			}
		}


		// copy up-to-date data
		Cell::set_transfer_all(
			true,
			pamhd::mhd::MHD_State_Conservative(),
			pamhd::mhd::HD1_State(),
			pamhd::mhd::HD2_State(),
			pamhd::mhd::Cell_Type()
		);
		grid.update_copies_of_remote_neighbors();
		Cell::set_transfer_all(
			false,
			pamhd::mhd::MHD_State_Conservative(),
			pamhd::mhd::HD1_State(),
			pamhd::mhd::HD2_State(),
			pamhd::mhd::Cell_Type()
		);

		// set copy boundary cells' neighbors
		for (const auto& cell: field_copy_bdy.get_cells()) {
			const auto* const neighbors_of = grid.get_neighbors_of(cell);
			if (neighbors_of == NULL) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"No neighbors for cell: " << cell
					<< std::endl;
				abort();
			}

			std::vector<uint64_t> final_neighbors;
			for (const auto& neighbor: *neighbors_of) {
				if (neighbor == dccrg::error_cell) {
					continue;
				}

				const auto* const neighbor_data = grid[neighbor];
				if (neighbor_data == NULL) {
					std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
						<< "No data for "
						<< (grid.is_local(neighbor) ? "local" : "remote")
						<< " neighbor: " << neighbor
						<< " of cell " << cell
						<< std::endl;
					abort();
				}

				if ((*neighbor_data)[pamhd::mhd::Cell_Type()] == 0) {
					final_neighbors.push_back(neighbor);
				}
			}

			if (field_copy_bdy.set_neighbors_of(cell, final_neighbors) > 1) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "> 1 non-boundary cell assigned as source for cell " << cell
					<< ": " << field_copy_bdy.get_source_cells(cell)
					<< std::endl;
				abort();
			}
		}

		/*
		Remove field copy cells without normal neighbors from cell lists
		so their flux isn't applied
		*/
		std::unordered_set<uint64_t>
			temp_inner(mhd_inner_cell_ids.cbegin(), mhd_inner_cell_ids.cend()),
			temp_outer(mhd_outer_cell_ids.cbegin(), mhd_outer_cell_ids.cend());

		for (const auto& cell: field_copy_bdy.get_cells()) {
			const auto& source = field_copy_bdy.get_source_cells(cell);
			if (source.size() > 1) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Cell " << cell << " has more than one source cell."
					<< std::endl;
				abort();
			}

			if (source.size() > 0) {
				continue;
			}

			temp_inner.erase(cell);
			temp_outer.erase(cell);
		}
		mhd_inner_cell_ids.clear();
		mhd_outer_cell_ids.clear();
		mhd_inner_cell_ids.insert(mhd_inner_cell_ids.end(), temp_inner.cbegin(), temp_inner.cend());
		mhd_outer_cell_ids.insert(mhd_outer_cell_ids.end(), temp_outer.cbegin(), temp_outer.cend());
		temp_inner.clear();
		temp_outer.clear();

		// copy data to copy boundary cells
		for (const auto& cell: field_copy_bdy.get_cells()) {
			const auto& source = field_copy_bdy.get_source_cells(cell);
			if (source.size() > 1) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "Cell " << cell << " has more than one source cell."
					<< std::endl;
				abort();
			}

			if (source.size() == 0) {
				continue;
			}

			auto
				*const target_data = grid[cell],
				*const source_data = grid[source[0]];

			if (source_data == NULL or target_data == NULL) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					<< "No data for source or target cell."
					<< std::endl;
				abort();
			}

			Mas(*target_data) = Mas(*source_data);
			Mas1(*target_data) = Mas1(*source_data);
			Mom(*target_data) = Mom(*source_data);
			Mom1(*target_data) = Mom1(*source_data);
			Nrj(*target_data) = Nrj(*source_data);
			Nrj1(*target_data) = Nrj1(*source_data);
			Mag(*target_data) = Mag(*source_data);
		}


		/*
		Save result(s) to file(s)
		*/
		if (
			(save_particle_n >= 0 and (simulation_time == 0 or simulation_time >= end_time))
			or (save_particle_n > 0 and simulation_time >= next_particle_save)
		) {
			if (next_particle_save <= simulation_time) {
				next_particle_save += save_particle_n;
			}

			if (verbose && rank == 0) {
				cout << "Saving particles at time " << simulation_time << endl;
			}

			if (
				not pamhd::particle::save<
					pamhd::particle::Electric_Field,
					pamhd::particle::Magnetic_Field,
					pamhd::particle::Nr_Particles_Internal,
					pamhd::particle::Particles_Internal
				>(
					output_directory,
					grid,
					simulation_time,
					adiabatic_index,
					vacuum_permeability,
					particle_temp_nrj_ratio
				)
			) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't save particle result."
					<< std::endl;
				MPI_Finalize();
				return EXIT_SUCCESS;
			}

			if (
				not pamhd::mhd::Save::save(
					output_directory + "mhd_",
					grid,
					simulation_time,
					adiabatic_index,
					proton_mass,
					vacuum_permeability,
					pamhd::mhd::MHD_State_Conservative(),
					pamhd::mhd::Electric_Current_Density()
				)
			) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't save mhd result."
					<< std::endl;
				abort();
			}

			if (
				not pamhd::mhd::Save::save(
					output_directory + "hd1_",
					grid,
					simulation_time,
					adiabatic_index,
					proton_mass,
					vacuum_permeability,
					pamhd::mhd::HD1_State()
				)
			) {
				std::cerr <<  __FILE__ << "(" << __LINE__ << "): "
					"Couldn't save fluid1 result."
					<< std::endl;
				abort();
			}
		}
	}

	MPI_Finalize();

	return EXIT_SUCCESS;
}
