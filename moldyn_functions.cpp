#include "moldyn_functions.hpp"

double get_time() {
	return (double) chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count()/1000.0;
}

void printray(vec3 ray) {
	printf("%.18f %.18f %.18f\n", ray.x, ray.y, ray.z);
}

double get_length_sqrd(vec3 ray) {
	return pow(ray.x, 2) + pow(ray.y, 2) + pow(ray.z, 2);
}

void get_atom_combinations(vector<pair<Atom*, Atom*>> &combinations, vector<Atom> &atoms) {
	string bitmask(2, 1); // 2 leading 1's
	bitmask.resize(atoms.size(), 0); // N-2 trailing 0's

	int i = 0;
	do {
		bool first = true;
		for (int j = 0; j < atoms.size(); j++) {
			if (bitmask[j]) {
				if (first) {
					combinations[i].first = &atoms[j];
					first = false;
				} else {
					combinations[i].second = &atoms[j];
				}
			}
		}
		i++;
	} while (prev_permutation(bitmask.begin(), bitmask.end()));
}


double U(double r_sqrd) {
	return 4*(pow(r_sqrd, -6) - pow(r_sqrd, -3)) + 2912/531441.0;
}

vec3 get_force(vec3 &between_vec, double r_sqrd) {
	vec3 direction_vec = between_vec/r_sqrd;
	double force = 24*(2*pow(r_sqrd, -6) - pow(r_sqrd, -3));

	return direction_vec*force;
}


tuple<vec3, double> periodic_boundry(vec3 &between_ray, double L) {
	double dx = between_ray[0];
	dx = dx - round(dx/L)*L;
	double dy = between_ray[1];
	dy = dy - round(dy/L)*L;
	double dz = between_ray[2];
	dz = dz - round(dz/L)*L;

	vec3 direction_ray = {dx, dy, dz};
	double r_sqrd = pow(dx, 2) + pow(dy, 2) + pow(dz, 2);

	return tuple<vec3, double>(direction_ray, r_sqrd);
}

tuple<double, double, double> get_energy(vector<Atom> &atoms, vector<pair<Atom*, Atom*>> &atom_combinations, double L) {
	// Calculate potential energy:
	double potential_energy = 0;
	for (pair<Atom*, Atom*> &atom_combination : atom_combinations) {
		Atom *atom1 = atom_combination.first;
		Atom *atom2 = atom_combination.second;

		vec3 between_vec = atom1->pos - atom2->pos;
		auto [direction_vec, r_sqrd] = periodic_boundry(between_vec, L);
		potential_energy += U(r_sqrd);
	}

	// Calculate kinetic energy:
	double kinetic_energy = 0;
	for (Atom &atom : atoms) {
		kinetic_energy += 0.5*get_length_sqrd(atom.vel);
	}

	return tuple<double, double, double>(potential_energy, kinetic_energy, potential_energy+kinetic_energy);
}


void ez_simulate(vector<Atom> &atoms, vector<pair<Atom*, Atom*>> &atom_combinations, double dt, double t_max, double L) {
	vector<double> t_list((int) t_max/dt, 0);

	FILE* datafile = fopen("data/null", "w");

	for (size_t i = 0; i < t_list.size(); i++) {
		t_list[i] = i*dt;
		// Fancy progress indicator
		if ((int) (t_list[i]*6/t_max*10) % 10 == 0) {
			cout << "\r";
			for (int j = 0; j < t_list[i]*6/t_max; j++) {
				cout << ".";
			}
			cout << flush;
		}
		step(atoms, atom_combinations, dt, L, datafile);
	}

	fclose(datafile);
}

tuple<vector<double>, vector<double>, vector<double>, vector<double>, vector<double>, vector<double>, vector<double>> simulate(
	vector<Atom> &atoms, vector<pair<Atom*, Atom*>> &atom_combinations, double dt, double t_max, string filename, double L, int completion, int total_runs
) {
	// Declare variables
	int n = t_max/dt;
	vector<double> t_list(n, 0);
	vector<double> pot_list(n, 0);
	vector<double> kin_list(n, 0);
	vector<double> tot_list(n, 0);
	vector<double> tmp_list(n, 0);
	vector<double> vac_list(n, 0);
	vector<double> msd_list(n, 0);

	FILE *datafile = fopen(("data/"+filename+".xyz").c_str(), "w");

	for (size_t i = 0; i < t_list.size(); i++) {
		t_list[i] = i*dt;
		// Fancy progress indicator, now even more complicated
		double run_percent = (100*completion + t_list[i]*100/t_max)/total_runs;
		if ((int) (run_percent*10) % 10 == 0) {
			printf("\r...... %i/%i : %3.0f %%", completion+1, total_runs, run_percent);
			fflush(stdout);
		}

		// Get and store energy
		auto [pot, kin, tot] = get_energy(atoms, atom_combinations, L);
		pot_list[i] = pot;
		kin_list[i] = kin;
		tot_list[i] = tot;

		// Calculate and store temperature
		double temperature = (2.0/(3*atoms.size()))*kin;
		tmp_list[i] = temperature;

		// Calculate and store velocity autocorrelation
		double vac = 0;
		for (Atom &atom : atoms) {
			double vac_emum = glm::dot(atom.vel, atom.vel0);
			double vac_denom = get_length_sqrd(atom.vel0);
			vac += vac_emum/vac_denom;
		}
		vac = vac/((double) atoms.size());
		vac_list[i] = vac;

		// Calculate and store mean squared displacement
		double msd = 0;
		for (Atom &atom : atoms) {
			msd += get_length_sqrd(atom.dist_traveled(L) - atom.pos0);
		}
		msd = msd/((double) atoms.size());
		msd_list[i] = msd;

		step(atoms, atom_combinations, dt, L, datafile);
	}

	fclose(datafile);
	return make_tuple(t_list, pot_list, kin_list, tot_list, tmp_list, vac_list, msd_list);
}

void step(vector<Atom> &atoms, vector<pair<Atom*, Atom*>> &atom_combinations, double dt, double L, FILE *datafile) {
	// Add the force acting on the particles efficiently using pairs
	for (pair<Atom*, Atom*> &atom_combination : atom_combinations) {
		Atom *atom1 = atom_combination.first;
		Atom *atom2 = atom_combination.second;

		vec3 between_ray = atom1->pos - atom2->pos;
		auto [direction_ray, r_sqrd] = periodic_boundry(between_ray, L);

		if (r_sqrd < 3*3) {
			vec3 force = get_force(direction_ray, r_sqrd);
			atom1->force += force;
			atom2->force -= force;
		}
	}

	fprintf(datafile, "%zi\ntype x y z\n", atoms.size());
	for (Atom &atom : atoms) {
		// Save current atom positions to file
		atom.save_state(datafile);
		// Update atom positions using given method
		atom.update(dt, L);
	}
}


vector<vec3> box_positions(int n, double d) {
	vector<vec3> positions;
	positions.reserve(4*pow(n, 3));
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			for (int k = 0; k < n; k++) {
				positions.push_back(vec3(i, j, k)*d);
				positions.push_back(vec3(i, 0.5 + j, 0.5 + k)*d);
				positions.push_back(vec3(0.5 + i, j, 0.5 + k)*d);
				positions.push_back(vec3(0.5 + i, 0.5 + j, k)*d);
			}
		}
	}
	return positions;
}

tuple<double, vector<Atom>, vector<pair<Atom*, Atom*>>> create_atoms(int atom_count, double d, double temperature) {
	int n = cbrt(atom_count/4.0);
	double L = d*n;

	vector<Atom> atoms;
	atoms.reserve(atom_count);

	default_random_engine generator(chrono::system_clock::now().time_since_epoch().count());
	normal_distribution<double> normal_temperature(0, sqrt(temperature/119.7));

	// string line;
	// ifstream normal_speeds("data/normal_speeds.dat");
	// vector<string> str_list;
	// while (getline(normal_speeds, line)) {
	// 	str_list.push_back(line);
	// }

	vector<vec3> positions = box_positions(n, d);
	for (int i = 0; i < atom_count; i++) {
		// vector<string> elements = split(str_list[i], ", ");
		vec3 velocitiy = vec3(normal_temperature(generator), normal_temperature(generator), normal_temperature(generator));
		atoms.push_back(Atom(positions[i], velocitiy));
	}

	// normal_speeds.close();

	int atom_combination_count = (atom_count*(atom_count-1))/2;
	vector<pair<Atom*, Atom*>> atom_combinations(atom_combination_count);
	get_atom_combinations(atom_combinations, atoms);

	// for (Atom &atom : atoms) {
	// 	printray(atom.vel);
	// }

	return tuple<double, vector<Atom>, vector<pair<Atom*, Atom*>>>(L, move(atoms), move(atom_combinations));
}

tuple<double, vector<Atom>, vector<pair<Atom*, Atom*>>> create_equalibrium_atoms(int atom_count, double d, double temperature, double dt, double t_max) {
	auto [L, warm_atoms, atom_combinations] = create_atoms(atom_count, d, temperature);
	ez_simulate(warm_atoms, atom_combinations, dt, t_max, L);
	for (Atom &atom : warm_atoms) {
		atom.vel0 = atom.vel;
		atom.pos0 = atom.pos;
	}

	return tuple<double, vector<Atom>, vector<pair<Atom*, Atom*>>>(L, move(warm_atoms), move(atom_combinations));
}
