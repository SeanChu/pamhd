#! /usr/bin/env python
'''
Converts realtime ACE data from NOAA to format suitable for MHD test program of PAMHD.

Copyright 2015 Ilja Honkonen
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
'''

from argparse import ArgumentParser
from datetime import datetime
from ftplib import FTP

def get_plasma_data():
	mag_lines = []
	swepam_lines = []
	conn = FTP('ftp.swpc.noaa.gov')
	conn.login()
	conn.retrlines('RETR pub/lists/ace/ace_mag_1m.txt', mag_lines.append)
	conn.retrlines('RETR pub/lists/ace/ace_swepam_1m.txt', swepam_lines.append)

	mag_data = []
	for line in mag_lines:
		line = line.strip()
		if line.startswith(':') or line.startswith('#'):
			continue

		yyyy, mm, dd, hhmm, mjd, sof, status, bx, by, bz, bt, lat, lon = line.split()

		if status != '0':
			continue

		if bx == '-999.9' or by == '-999.9' or bz == '-999.9':
			continue

		dt = datetime(int(yyyy), int(mm), int(dd), int(hhmm[0:2]), int(hhmm[2:4]))
		bx, by, bz = float(bx)*1e-9, float(by)*1e-9, float(bz)*1e-9
		mag_data.append((dt, bx, by, bx))

	# use average value of Bx
	Bx_avg = 0
	for item in mag_data:
		Bx_avg += item[1]
	Bx_avg /= len(mag_data)

	swepam_data = []
	for line in swepam_lines:
		line = line.strip()
		if line.startswith(':') or line.startswith('#'):
			continue

		yyyy, mm, dd, hhmm, mjd, sof, status, density, speed, temperature = line.split()

		if status != '0':
			continue

		if density == '-9999.9' or speed == '-9999.9' or temperature == '-1.00e+05':
			continue

		dt = datetime(int(yyyy), int(mm), int(dd), int(hhmm[0:2]), int(hhmm[2:4]))
		density, speed, temperature = float(density)*1e6, -float(speed)*1e3, float(temperature)
		if density <= 0:
			density = 5e4
		swepam_data.append((dt, density, speed, temperature ))

	merged_data = []
	mag_i = 0
	swepam_i = 0
	while mag_i < len(mag_data) and swepam_i < len(swepam_data):
		if mag_data[mag_i][0] == swepam_data[swepam_i][0]:
			merged_data.append((mag_data[mag_i][0:1] + (Bx_avg,) + mag_data[mag_i][2:] + swepam_data[swepam_i][1:]))
			mag_i += 1
			swepam_i += 1
		elif mag_data[mag_i][0] < swepam_data[swepam_i][0]:
			mag_i += 1
		elif mag_data[mag_i][0] > swepam_data[swepam_i][0]:
			swepam_i += 1

	return merged_data


def get_pressure(number_density, temperature):
	particle_temp_nrj_ratio = 1.3806488e-23
	return number_density * temperature * particle_temp_nrj_ratio


if __name__ == '__main__':

	parser = ArgumentParser(
		description
			= 'Convert real-time solar wind from NOAA into a 1d '
			+ 'run configuration for MHD test of PAMHD'
	)
	parser.add_argument(
		'--config',
		metavar = 'C',
		default = '',
		help = 'Write configuration to file with name C (if empty '
			+ 'use first time stamp of data in ISO 8601 format)'
	)
	parser.add_argument(
		'--model-output',
		metavar = 'O',
		default = '',
		help = 'Output model results into directory O (if empty '
			+ 'use first time stamp of data in near ISO 8601 format)'
	)
	parser.add_argument(
		'--cells',
		metavar = 'N',
		type = int,
		default = 1000,
		help = 'Use N simulation cells'
	)
	parser.add_argument(
		'--length',
		metavar = 'L',
		type = float,
		default = 1e9,
		help = 'Use simulation box of length L (m)'
	)
	parser.add_argument(
		'--duration',
		metavar = 'T',
		type = float,
		default = 1e4,
		help = 'Simulate solar wind for duration T (s)'
	)

	args = parser.parse_args()

	plasma_data = get_plasma_data()
	time_start = plasma_data[0][0]
	time_length = plasma_data[-1][0] - time_start
	time_length = time_length.days * 60*60*24 + time_length.seconds

	config_file = None
	if args.config == '':
		config_file = open('config-' + time_start.isoformat(), 'w')
	else:
		config_file = open(args.config, 'w')

	if args.model_output == '':
		config_file.write('output-directory = ' + time_start.isoformat().replace(':', ''))
	else:
		config_file.write(args.model_output)

	config_file.write(
		'\nsolver-mhd = roe_athena\n'
		+ 'time-start = 0\n'
		+ 'time-length = ' + str(args.duration)
		+ '\nload-balancer = RCB\n'
		+ 'save-mhd-n = 60\n'
		+ 'remove-div-B-n = -1\n'
		+ '[grid]\n'
		+ 'periodic = {false, false, false}\n'
		+ 'nr-cells = {' + str(args.cells) + ' + 2, 1, 1}\n'
		+ 'volume = {' + str(args.length)
		+ ' * (1 + 2 / ' + str(args.cells)
		+ '), ' + str(args.length)
		+ ', ' + str(args.length)
		+ '}\nstart = {-1 * ' + str(args.length)
		+ ' / ' + str(args.cells) + ', -'
		+ str(args.length / 2) + ', -'
		+ str(args.length / 2)
		+ '}\n[initial]\n'
		+ 'default.number-density = ' + str(plasma_data[0][4])
		+ '\ndefault.velocity = {' + str(plasma_data[0][5]) + ', 0, 0}\n'
		+ 'default.pressure = ' + str(get_pressure(plasma_data[0][4], plasma_data[0][6]))
		+ '\ndefault.magnetic-field = {' + str(plasma_data[0][1])
		+ ', ' + str(plasma_data[0][2])
		+ ', ' + str(plasma_data[0][3])
		+ '}\n[copy-boundaries]\nnr-boxes = 1\n'
		+ 'box1.start = {-9e999, -9e999, -9e999}\n'
		+ 'box1.end = {0, 9e999, 9e999}\n[value-boundaries]\nnr-boxes = 1\n'
	)

	for item in plasma_data:
		time = item[0] - time_start
		time = time.days * 60*60*24 + time.seconds
		config_file.write(
			'box1.start = {' + str(args.length) + ', -9e999, -9e999}\n'
			+ 'box1.end = {9e999, 9e999, 9e999}\nbox1.time = ' + str(time)
			+ '\nbox1.number-density = ' + str(item[4])
			+ '\nbox1.velocity = {' + str(item[5])
			+ ', 0, 0}\nbox1.pressure = ' + str(get_pressure(item[4], item[6]))
			+ '\nbox1.magnetic-field = {' + str(item[1])
			+ ', ' + str(item[2])
			+ ', ' + str(item[3]) + '}\n'
		)
