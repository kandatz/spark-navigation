/*
 *  Player - One Hell of a Robot Server
 *  Copyright (C) 2003
 *     Andrew Howard
 *     Brian Gerkey    
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */


/**************************************************************************
 * Desc: Path planning
 * Author: Andrew Howard
 * Date: 10 Oct 2002
 * CVS: $Id: plan.h 9139 2014-02-18 02:50:05Z jpgr87 $
**************************************************************************/

#ifndef PLAN_H
#define PLAN_H

#include <queue>
#include <vector>

#define PLAN_MAX_COST 1e9

// Description for a grid single cell
typedef struct _plan_cell_t
{
  // Cell index in grid map
  unsigned short ci, cj;
  
  // Occupancy state (-1 = free, 0 = unknown, +1 = occ)
  char occ_state;
  char occ_state_dyn;

  // Distance to the nearest occupied cell
  float occ_dist;
  float occ_dist_dyn;

  // Distance (cost) to the goal
  float plan_cost;

  // Mark used in dynamic programming
  bool mark;
  // Mark used in path hysteresis
  bool lpathmark;

  // The next cell in the plan
  struct _plan_cell_t *plan_next;

} plan_cell_t;

// For std::priority_queue comparison.
namespace std {
	template<>
	struct less<plan_cell_t *>
	{
	   bool operator()(const plan_cell_t * c1, const plan_cell_t * c2) const
	   {
		   // We want a min-heap, to take the opposite of a 'less'.
		   return (c1->plan_cost > c2->plan_cost);
	   }
	};
}

template <typename T>
struct pos2d {
	T x, y;
};

// Planner info
struct plan_t
{
  // Create a planner
  plan_t(double abs_min_radius,
         double max_radius,
         double dist_penalty,
         double hysteresis_factor);

  void compute_dist_kernel();

  // Copy a planner
  plan_t( const plan_t &obj);

  // Destroy a planner
  ~plan_t();

  // Initialize the plan
  void init();

  // Reset the plan
  void reset();

  #if 0
  // Load the occupancy values from an image file
  int load_occ(const char *filename, double scale);
  #endif

  void set_bbox(double padding, double min_size,
                double x0, double y0, double x1, double y1);

  bool check_inbounds(double x, double y) const;

  // Construct the configuration space from the occupancy grid.
  //void plan_update_cspace(plan_t *plan, const char* cachefile);
  void compute_cspace();

  // Return true if succeeded.
  bool do_global(const pos2d<double> & l, const pos2d<double> & g);

  // Return true if succeeded.
  bool do_local(const pos2d<double> & l, double plan_halfwidth);

  // Generate a path to the goal
  void update_waypoints(const pos2d<double> & p);

  // Convert given waypoint cell to global x,y
  void convert_waypoint(const plan_cell_t & waypoint,
                        double *px, double *py) const;

  double get_carrot(double* px, double* py,
                    double lx, double ly,
                    double maxdist, double distweight);

  int compute_diffdrive_cmds(double* vx, double *va,
                             int* rotate_dir,
                             double lx, double ly, double la,
                             double gx, double gy, double ga,
                             double goal_d, double goal_a,
                             double maxd, double dweight,
                             double tvmin, double tvmax,
                             double avmin, double avmax,
                             double amin, double amax);

  void set_obstacles(double* obs, size_t num);

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
// Write the cspace occupancy distance values to a file, one per line.
// Read them back in with plan_read_cspace().
// Returns non-zero on error.
int write_cspace(const char* fname, unsigned int* hash);

// Read the cspace occupancy distance values from a file, one per line.
// Write them in first with plan_read_cspace().
// Returns non-zero on error.
int read_cspace(const char* fname, unsigned int* hash);

// Compute and return the 16-bit MD5 hash of the map data in the given plan
// object.
void md5(unsigned int* digest) const;
#endif // HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO

  // Grid dimensions (number of cells)
  pos2d<int> size;

  // Grid origin (real-world coords, in meters, of the lower-left grid cell)
  pos2d<double> origin;

  // Grid scale (m/cell)
  double scale;

  // Max radius we will consider
  double max_radius;

  // The grid data
  plan_cell_t *cells;

  // The global path
  std::vector<plan_cell_t *> path;

  // The local path (mainly for debugging)
  std::vector<plan_cell_t *> lpath;

  // Waypoints extracted from global path
  typedef std::vector<plan_cell_t *> waypoints_t;
  waypoints_t waypoints;

private:
  bool update_plan(const pos2d<double> & l, const pos2d<double> & g);
  int find_local_goal(pos2d<double> * g, const pos2d<double> & l) const;

  // FIXME: return int?
  double check_path(const plan_cell_t & s, const plan_cell_t & g) const;

  // Test to see if once cell is reachable from another
  bool test_reachable(const plan_cell_t & cell_a, const plan_cell_t & cell_b) const;

  // Get the ith waypoint; returns false if there are no more waypoints
  bool get_waypoint(waypoints_t::size_type i, double *px, double *py) const;

  bool check_done(double lx, double ly, double la,
                  double gx, double gy, double ga,
                  double goal_d, double goal_a) const;

  void set_bounds(int min_x, int min_y, int max_x, int max_y);

  // Priority queue of cells to update
  std::priority_queue<plan_cell_t *> heap;

  // Plan queue stuff
  void push(plan_cell_t *cell);
  plan_cell_t *pop();

  // Distance penalty kernel, pre-computed in plan_compute_dist_kernel();
  float* dist_kernel;
  int dist_kernel_width;
  float dist_kernel_3x3[9];

  // Penalty factor for cells inside the max radius
  double dist_penalty;

  // Cost multiplier for cells on the previous local path
  double hysteresis_factor;

  // Grid bounds (for limiting the search).
  int min_x, min_y, max_x, max_y;

  // Effective robot radius
  double abs_min_radius;

  // Convert from world coords to plan coords
  int GXWX(double x) const;
  int GYWY(double y) const;

  // Test to see if the given plan coords lie within the absolute plan bounds.
  bool VALID(int i, int j) const;

  // Test to see if the given plan coords lie within the user-specified plan bounds
  bool VALID_BOUNDS(int i, int j) const;

  static double ANG_NORM(double a);

public:
  // Convert from plan index to world coords
  double WXGX(int i) const;
  double WYGY(int j) const;

  // Compute the cell index for the given plan coords.
  int INDEX(int i, int j) const;

  // computes the signed minimum difference between the two angles.
  static double angle_diff(double a, double b);

  static double get_time(void);
};

#endif
