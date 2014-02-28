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
 * CVS: $Id: plan.c 9139 2014-02-18 02:50:05Z jpgr87 $
**************************************************************************/

//#include <config.h>

// This header MUST come before <openssl/md5.h>
#include <sys/types.h>

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  #include <openssl/md5.h>
#endif

#include <cassert>
#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <libplayercommon/playercommon.h>

#include "plan.h"

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
// length of the hash, in unsigned ints
#define HASH_LEN (MD5_DIGEST_LENGTH / sizeof(unsigned int))
#endif

#if defined (WIN32)
  #include <replace/replace.h>
  #include <winsock2.h> // For struct timeval
#else
  #include <sys/time.h>
#endif
static double get_time(void);

#if 0
void draw_cspace(plan_t* plan, const char* fname);
#endif

// Create a planner
plan_t::plan_t(double _abs_min_radius, double _des_min_radius,
               double _max_radius, double _dist_penalty,
               double _hysteresis_factor) :
		abs_min_radius(_abs_min_radius),
		des_min_radius(_des_min_radius),
		max_radius(_max_radius),
		dist_penalty(_dist_penalty),
		hysteresis_factor(_hysteresis_factor),
		path_size(1000),
		lpath_size(100),
	 	waypoint_size(100),
	 	min_x(0), min_y(0), max_x(0), max_y(0),
	 	size_x(0), size_y(0),
	 	origin_x(0), origin_y(0),
	 	scale(0.0),
	 	cells(NULL),
	 	path_count(0), lpath_count(0), waypoint_count(0),
	 	dist_kernel(NULL), dist_kernel_width(0)
{
  this->path = (plan_cell_t **) calloc(this->path_size, sizeof(path[0]));
  this->lpath = (plan_cell_t **) calloc(this->lpath_size, sizeof(lpath[0]));
  this->waypoints = (plan_cell_t **) calloc(this->waypoint_size, sizeof(waypoints[0]));

  assert(this->path);
  assert(this->lpath);
  assert(this->waypoints);
}

// Destroy a planner
plan_t::~plan_t()
{
  if (cells)
    free(cells);
  free(waypoints);
  if(dist_kernel)
    free(dist_kernel);
}

// Copy the planner
plan_t::plan_t(const plan_t & plan) :
	abs_min_radius(plan.abs_min_radius),
	des_min_radius(plan.des_min_radius),
	max_radius(plan.max_radius),
	dist_penalty(plan.dist_penalty),
	hysteresis_factor(plan.hysteresis_factor),
	path_size(1000),
	lpath_size(100),
	waypoint_size(100),
	min_x(0), min_y(0), max_x(0), max_y(0),
	size_x(plan.size_x), size_y(plan.size_y),
	origin_x(plan.origin_x), origin_y(plan.origin_y),
	scale(plan.scale),
	cells(NULL),
	path_count(0), lpath_count(0), waypoint_count(0),
	dist_kernel(NULL), dist_kernel_width(0)
{
  this->path = (plan_cell_t **) calloc(this->path_size, sizeof(path[0]));
  this->lpath = (plan_cell_t **) calloc(this->lpath_size, sizeof(lpath[0]));
  this->waypoints = (plan_cell_t **) calloc(this->waypoint_size, sizeof(waypoints[0]));

  assert(this->path);
  assert(this->lpath);
  assert(this->waypoints);

  // Now get the map data
  // Allocate space for map cells
  this->cells = (plan_cell_t*)malloc((this->size_x *
                                      this->size_y *
                                      sizeof(plan_cell_t)));
  assert(this->cells);

  // Do initialization
  this->init();

  // Copy the map data
  for (int i = 0; i < this->size_x * this->size_y; ++i)
  {
    this->cells[i].occ_dist = plan.cells[i].occ_dist;
	this->cells[i].occ_state = plan.cells[i].occ_state;
	this->cells[i].occ_state_dyn = plan.cells[i].occ_state_dyn;
	this->cells[i].occ_dist_dyn = plan.cells[i].occ_dist_dyn;
  }
}

void
plan_t::set_obstacles(double* obs, size_t num)
{
  double t0 = get_time();

  // Start with static obstacle data
  plan_cell_t* cell = cells;
  for(int j=0;j<size_y*size_x;j++,cell++)
  {
    cell->occ_state_dyn = cell->occ_state;
    cell->occ_dist_dyn = cell->occ_dist;
    cell->mark = 0;
  }

  // Expand around the dynamic obstacle pts
  for(size_t i=0;i<num;i++)
  {
    // Convert to grid coords
    int gx = PLAN_GXWX(this, obs[2*i]);
    int gy = PLAN_GYWY(this, obs[2*i+1]);

    if(!PLAN_VALID(this,gx,gy))
      continue;

    cell = cells + PLAN_INDEX(this,gx,gy);

    if(cell->mark)
      continue;

    cell->mark = 1;
    cell->occ_state_dyn = 1;
    cell->occ_dist_dyn = 0.0;

    float * p = dist_kernel;
    for (int dj = -dist_kernel_width/2;
             dj <= dist_kernel_width/2;
             dj++)
    {
      plan_cell_t * ncell = cell + -dist_kernel_width/2 + dj*size_x;
      for (int di = -dist_kernel_width/2;
               di <= dist_kernel_width/2;
               di++, p++, ncell++)
      {
        if(!PLAN_VALID_BOUNDS(this,cell->ci+di,cell->cj+dj))
          continue;

        if(*p < ncell->occ_dist_dyn)
          ncell->occ_dist_dyn = *p;
      }
    }
  }

  double t1 = get_time();
  //printf("plan_set_obstacles: %.6lf\n", t1-t0);
}

void
plan_t::compute_dist_kernel()
{
  // Compute variable sized kernel, for use in propagating distance from
  // obstacles
  dist_kernel_width = 1 + 2 * (int)ceil(max_radius / scale);
  dist_kernel = (float*)realloc(dist_kernel,
                                sizeof(float) *
                                  dist_kernel_width *
                                  dist_kernel_width);
  assert(dist_kernel);

  float * p = dist_kernel;
  for(int j=-dist_kernel_width/2;j<=dist_kernel_width/2;j++)
  {
    for(int i=-dist_kernel_width/2;i<=dist_kernel_width/2;i++,p++)
    {
      *p = (float) (sqrt(i*i+j*j) * scale);
    }
  }
  // also compute a 3x3 kernel, used when propagating distance from goal
  p = dist_kernel_3x3;
  for(int j=-1;j<=1;j++)
  {
    for(int i=-1;i<=1;i++,p++)
    {
      *p = (float) (sqrt(i*i+j*j) * scale);
    }
  }
}

// Initialize the plan
void plan_t::init()
{
  printf("scale: %.3lf\n", scale);

  plan_cell_t *cell = cells;
  for (int j = 0; j < size_y; j++)
  {
    for (int i = 0; i < size_x; i++, cell++)
    {
      cell->ci = i;
      cell->cj = j;
      cell->occ_state_dyn = cell->occ_state;
      if(cell->occ_state >= 0)
        cell->occ_dist_dyn = cell->occ_dist = 0.0;
      else
        cell->occ_dist_dyn = cell->occ_dist = (float) (max_radius);
      cell->plan_cost = PLAN_MAX_COST;
      cell->plan_next = NULL;
      cell->lpathmark = 0;
    }
  }
  waypoint_count = 0;

  compute_dist_kernel();

  set_bounds(0, 0, size_x - 1, size_y - 1);
}


// Reset the plan
void plan_t::reset()
{
  for (int j = min_y; j <= max_y; j++)
  {
    for (int i = min_x; i <= max_x; i++)
    {
      plan_cell_t *cell = cells + PLAN_INDEX(this,i,j);
      cell->plan_cost = PLAN_MAX_COST;
      cell->plan_next = NULL;
      cell->mark = 0;
    }
  }
  waypoint_count = 0;
}

void
plan_t::set_bounds(int min_x, int min_y, int max_x, int max_y)
{
  // TODO: name clashes with member data?
  min_x = MAX(0,min_x);
  min_x = MIN(size_x-1, min_x);
  min_y = MAX(0,min_y);
  min_y = MIN(size_y-1, min_y);
  max_x = MAX(0,max_x);
  max_x = MIN(size_x-1, max_x);
  max_y = MAX(0,max_y);
  max_y = MIN(size_y-1, max_y);

  assert(min_x <= max_x);
  assert(min_y <= max_y);

  this->min_x = min_x;
  this->min_y = min_y;
  this->max_x = max_x;
  this->max_y = max_y;

  //printf("new bounds: (%d,%d) -> (%d,%d)\n",
         //plan->min_x, plan->min_y,
         //plan->max_x, plan->max_y);
}

bool
plan_t::check_inbounds(double x, double y) const
{
  int gx = PLAN_GXWX(this, x);
  int gy = PLAN_GYWY(this, y);

  return ((gx >= min_x) && (gx <= max_x) &&
          (gy >= min_y) && (gy <= max_y));
}

void
plan_t::set_bbox(double padding, double min_size,
                 double x0, double y0, double x1, double y1)
{
  int gx0, gy0, gx1, gy1;
  int min_x, min_y, max_x, max_y;
  int sx, sy;
  int dx, dy;
  int gmin_size;
  int gpadding;

  gx0 = PLAN_GXWX(this, x0);
  gy0 = PLAN_GYWY(this, y0);
  gx1 = PLAN_GXWX(this, x1);
  gy1 = PLAN_GYWY(this, y1);

  // Make a bounding box to include both points.
  min_x = MIN(gx0, gx1);
  min_y = MIN(gy0, gy1);
  max_x = MAX(gx0, gx1);
  max_y = MAX(gy0, gy1);

  // Make sure the min_size is achievable
  gmin_size = (int)ceil(min_size / scale);
  gmin_size = MIN(gmin_size, MIN(size_x-1, size_y-1));

  // Add padding
  gpadding = (int)ceil(padding / scale);
  min_x -= gpadding / 2;
  min_x = MAX(min_x, 0);
  max_x += gpadding / 2;
  max_x = MIN(max_x, size_x - 1);
  min_y -= gpadding / 2;
  min_y = MAX(min_y, 0);
  max_y += gpadding / 2;
  max_y = MIN(max_y, size_y - 1);

  // Grow the box if necessary to achieve the min_size
  sx = max_x - min_x;
  while(sx < gmin_size)
  {
    dx = gmin_size - sx;
    min_x -= (int)ceil(dx / 2.0);
    max_x += (int)ceil(dx / 2.0);

    min_x = MAX(min_x, 0);
    max_x = MIN(max_x, size_x-1);

    sx = max_x - min_x;
  }
  sy = max_y - min_y;
  while(sy < gmin_size)
  {
    dy = gmin_size - sy;
    min_y -= (int)ceil(dy / 2.0);
    max_y += (int)ceil(dy / 2.0);

    min_y = MAX(min_y, 0);
    max_y = MIN(max_y, size_y-1);

    sy = max_y - min_y;
  }

  set_bounds(min_x, min_y, max_x, max_y);
}

void
plan_t::compute_cspace()
{
  puts("Generating C-space....");

  for (int j = min_y; j <= max_y; j++)
  {
    plan_cell_t *cell = cells + PLAN_INDEX(this, 0, j);
    for (int i = min_x; i <= max_x; i++, cell++)
    {
      if (cell->occ_state < 0)
        continue;

      float *p = dist_kernel;
      for (int dj = -dist_kernel_width/2;
               dj <= dist_kernel_width/2;
               dj++)
      {
        plan_cell_t *ncell = cell + -dist_kernel_width/2 + dj*size_x;
        for (int di = -dist_kernel_width/2;
                 di <= dist_kernel_width/2;
                 di++, p++, ncell++)
        {
          if(!PLAN_VALID_BOUNDS(this,i+di,j+dj))
            continue;

          if(*p < ncell->occ_dist)
            ncell->occ_dist_dyn = ncell->occ_dist = *p;
        }
      }
    }
  }
}

#if 0
#include <gdk-pixbuf/gdk-pixbuf.h>

void
draw_cspace(plan_t* plan, const char* fname)
{
  GdkPixbuf* pixbuf;
  GError* error = NULL;
  guchar* pixels;
  int p;
  int paddr;
  int i, j;

  pixels = (guchar*)malloc(sizeof(guchar)*plan->size_x*plan->size_y*3);

  p=0;
  for(j=plan->size_y-1;j>=0;j--)
  {
    for(i=0;i<plan->size_x;i++,p++)
    {
      paddr = p * 3;
      //if(plan->cells[PLAN_INDEX(plan,i,j)].occ_state == 1)
      if(plan->cells[PLAN_INDEX(plan,i,j)].occ_dist < plan->abs_min_radius)
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 0;
      }
      else if(plan->cells[PLAN_INDEX(plan,i,j)].occ_dist < plan->max_radius)
      {
        pixels[paddr] = 0;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
      else
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 255;
        pixels[paddr+2] = 255;
      }
    }
  }

  pixbuf = gdk_pixbuf_new_from_data(pixels, 
                                    GDK_COLORSPACE_RGB,
                                    0,8,
                                    plan->size_x,
                                    plan->size_y,
                                    plan->size_x * 3,
                                    NULL, NULL);

  gdk_pixbuf_save(pixbuf,fname,"png",&error,NULL);
  gdk_pixbuf_unref(pixbuf);
  free(pixels);
}

        void
draw_path(plan_t* plan, double lx, double ly, const char* fname)
{
  GdkPixbuf* pixbuf;
  GError* error = NULL;
  guchar* pixels;
  int p;
  int paddr;
  int i, j;
  plan_cell_t* cell;

  pixels = (guchar*)malloc(sizeof(guchar)*plan->size_x*plan->size_y*3);

  p=0;
  for(j=plan->size_y-1;j>=0;j--)
  {
    for(i=0;i<plan->size_x;i++,p++)
    {
      paddr = p * 3;
      if(plan->cells[PLAN_INDEX(plan,i,j)].occ_state == 1)
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 0;
      }
      else if(plan->cells[PLAN_INDEX(plan,i,j)].occ_dist < plan->max_radius)
      {
        pixels[paddr] = 0;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
      else
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 255;
        pixels[paddr+2] = 255;
      }
      /*
         if((7*plan->cells[PLAN_INDEX(plan,i,j)].plan_cost) > 255)
         {
         pixels[paddr] = 0;
         pixels[paddr+1] = 0;
         pixels[paddr+2] = 255;
         }
         else
         {
         pixels[paddr] = 255 - 7*plan->cells[PLAN_INDEX(plan,i,j)].plan_cost;
         pixels[paddr+1] = 0;
         pixels[paddr+2] = 0;
         }
       */
    }
  }

  for(i=0;i<plan->path_count;i++)
  {
    cell = plan->path[i];
    
    paddr = 3*PLAN_INDEX(plan,cell->ci,plan->size_y - cell->cj - 1);
    pixels[paddr] = 0;
    pixels[paddr+1] = 255;
    pixels[paddr+2] = 0;
  }

  for(i=0;i<plan->lpath_count;i++)
  {
    cell = plan->lpath[i];
    
    paddr = 3*PLAN_INDEX(plan,cell->ci,plan->size_y - cell->cj - 1);
    pixels[paddr] = 255;
    pixels[paddr+1] = 0;
    pixels[paddr+2] = 255;
  }

  /*
  for(p=0;p<plan->waypoint_count;p++)
  {
    cell = plan->waypoints[p];
    for(j=-3;j<=3;j++)
    {
      cj = cell->cj + j;
      for(i=-3;i<=3;i++)
      {
        ci = cell->ci + i;
        paddr = 3*PLAN_INDEX(plan,ci,plan->size_y - cj - 1);
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
    }
  }
  */

  pixbuf = gdk_pixbuf_new_from_data(pixels, 
                                    GDK_COLORSPACE_RGB,
                                    0,8,
                                    plan->size_x,
                                    plan->size_y,
                                    plan->size_x * 3,
                                    NULL, NULL);
  
  gdk_pixbuf_save(pixbuf,fname,"png",&error,NULL);
  gdk_pixbuf_unref(pixbuf);
  free(pixels);
}
#endif

// Construct the configuration space from the occupancy grid.
// This treats both occupied and unknown cells as bad.
// 
// If cachefile is non-NULL, then we try to read the c-space from that
// file.  If that fails, then we construct the c-space as per normal and
// then write it out to cachefile.
#if 0
void 
plan_update_cspace(plan_t *plan, const char* cachefile)
{
#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  unsigned int hash[HASH_LEN];
  plan_md5(hash, plan);
  if(cachefile && strlen(cachefile))
  {
    PLAYER_MSG1(2,"Trying to read c-space from file %s", cachefile);
    if(plan_read_cspace(plan,cachefile,hash) == 0)
    {
      // Reading from the cache file worked; we're done here.
      PLAYER_MSG1(2,"Successfully read c-space from file %s", cachefile);
#if 0
      draw_cspace(plan,"plan_cspace.png");
#endif
      return;
    }
    PLAYER_MSG1(2, "Failed to read c-space from file %s", cachefile);
  }
#endif

  //plan_update_cspace_dp(plan);
  plan_update_cspace_naive(plan);

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  if(cachefile)
    plan_write_cspace(plan,cachefile, (unsigned int*)hash);
#endif

  PLAYER_MSG0(2,"Done.");

#if 0
  draw_cspace(plan,"plan_cspace.png");
#endif
}

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
// Write the cspace occupancy distance values to a file, one per line.
// Read them back in with plan_read_cspace().
// Returns non-zero on error.
int 
plan_write_cspace(plan_t *plan, const char* fname, unsigned int* hash)
{
  plan_cell_t* cell;
  int i,j;
  FILE* fp;

  if(!(fp = fopen(fname,"w+")))
  {
    PLAYER_MSG2(2,"Failed to open file %s to write c-space: %s",
                fname,strerror(errno));
    return(-1);
  }

  fprintf(fp,"%d\n%d\n", plan->size_x, plan->size_y);
  fprintf(fp,"%.3lf\n%.3lf\n", plan->origin_x, plan->origin_y);
  fprintf(fp,"%.3lf\n%.3lf\n", plan->scale,plan->max_radius);
  for(i=0;i<HASH_LEN;i++)
    fprintf(fp,"%08X", hash[i]);
  fprintf(fp,"\n");

  for(j = 0; j < plan->size_y; j++)
  {
    for(i = 0; i < plan->size_x; i++)
    {
      cell = plan->cells + PLAN_INDEX(plan, i, j);
      fprintf(fp,"%.3f\n", cell->occ_dist);
    }
  }

  fclose(fp);
  return(0);
}

// Read the cspace occupancy distance values from a file, one per line.
// Write them in first with plan_read_cspace().
// Returns non-zero on error.
int 
plan_read_cspace(plan_t *plan, const char* fname, unsigned int* hash)
{
  plan_cell_t* cell;
  int i,j;
  FILE* fp;
  int size_x, size_y;
  double origin_x, origin_y;
  double scale, max_radius;
  unsigned int cached_hash[HASH_LEN];

  if(!(fp = fopen(fname,"r")))
  {
    PLAYER_MSG1(2,"Failed to open file %s", fname);
    return(-1);
  }
  
  /* Read out the metadata */
  if((fscanf(fp,"%d", &size_x) < 1) ||
     (fscanf(fp,"%d", &size_y) < 1) ||
     (fscanf(fp,"%lf", &origin_x) < 1) ||
     (fscanf(fp,"%lf", &origin_y) < 1) ||
     (fscanf(fp,"%lf", &scale) < 1) ||
     (fscanf(fp,"%lf", &max_radius) < 1))
  {
    PLAYER_MSG1(2,"Failed to read c-space metadata from file %s", fname);
    fclose(fp);
    return(-1);
  }

  for(i=0;i<HASH_LEN;i++)
  {
    if(fscanf(fp,"%08X", cached_hash+i) < 1)
    {
      PLAYER_MSG1(2,"Failed to read c-space metadata from file %s", fname);
      fclose(fp);
      return(-1);
    }
  }

  /* Verify that metadata matches */
  if((size_x != plan->size_x) ||
     (size_y != plan->size_y) ||
     (fabs(origin_x - plan->origin_x) > 1e-3) ||
     (fabs(origin_y - plan->origin_y) > 1e-3) ||
     (fabs(scale - plan->scale) > 1e-3) ||
     (fabs(max_radius - plan->max_radius) > 1e-3) ||
     memcmp(cached_hash, hash, sizeof(unsigned int) * HASH_LEN))
  {
    PLAYER_MSG1(2,"Mismatch in c-space metadata read from file %s", fname);
    fclose(fp);
    return(-1);
  }

  for(j = 0; j < plan->size_y; j++)
  {
    for(i = 0; i < plan->size_x; i++)
    {
      cell = plan->cells + PLAN_INDEX(plan, i, j);
      if(fscanf(fp,"%f", &(cell->occ_dist)) < 1)
      {
        PLAYER_MSG3(2,"Failed to read c-space data for cell (%d,%d) from file %s",
                     i,j,fname);
        fclose(fp);
        return(-1);
      }
    }
  }

  fclose(fp);
  return(0);
}

// Compute the 16-byte MD5 hash of the map data in the given plan
// object.
void
plan_md5(unsigned int* digest, plan_t* plan)
{
  MD5_CTX c;

  MD5_Init(&c);

  MD5_Update(&c,(const unsigned char*)plan->cells,
             (plan->size_x*plan->size_y)*sizeof(plan_cell_t));

  MD5_Final((unsigned char*)digest,&c);
}
#endif // HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO

#endif // if 0

double 
static get_time(void)
{
  struct timeval curr;
  gettimeofday(&curr,NULL);
  return(curr.tv_sec + curr.tv_usec / 1e6);
}
