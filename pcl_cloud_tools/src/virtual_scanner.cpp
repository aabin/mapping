/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2010, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * $Id: virtual_scanner.cpp 36181 2010-04-27 16:08:14Z rusu $
 *
 */

/**
  * \author Radu Bogdan Rusu
  *
  * @b virtual_scanner takes in a .ply or a .vtk file of an object model, and virtually scans it
  * in a raytracing fashion, saving the end results as PCD (Point Cloud Data) files. In addition, 
  * it noisifies the PCD models, and downsamples them. 
  * The viewpoint can be set to 1 or multiple views on a sphere.
  */
volatile bool g_stopall = false;
#include <string>

// ROS messages
#include <ros/ros.h>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
// PCL dependencies
#include <pcl/ros/register_point_struct.h>
#include <pcl/io/pcd_io.h>
#include <pcl/filters/voxel_grid.h>

// ITPP dependencies (will go away after CVPR when we merge with trunk)
#include <itpp/itbase.h>
#include <itpp/itstat.h>
#include <itpp/signal/sigfun.h>

// VTK dependencies (will go away after CVPR when we merge with trunk)
#include <vtkMath.h>
#include <vtkGeneralTransform.h>
#include <vtkPlatonicSolidSource.h>
#include <vtkLoopSubdivisionFilter.h>
#include <vtkPLYReader.h>
#include <vtkSmartPointer.h>
#include <vtkCellLocator.h>
#include <vtkPolyData.h>

//terminal_tools includes
#include <terminal_tools/print.h>
#include <terminal_tools/parse.h>

//local includes
#include "pcl_cloud_tools/misc.h"

#include <vector>
#define EPS 0.00001

struct ScanParameters
{
  int nr_scans;             // number of steps for sweep movement
  int nr_points_in_scans;   // number of laser beam measurements per scan
  double vert_res;          // vertical resolution (res. of sweep) in degrees
  double hor_res;           // horizontal  resolution (of laser beam) in degrees
  double max_dist;          // maximum distance in units.
};

////////////////////////////////////////////////////////////////////////////////
/** \brief Loads a 3D point cloud from a given PLY fileName, and returns: a 
  * vtkDataSet object containing the point cloud.
  * \param file_name the name of the file containing the PLY dataset
  */
vtkPolyData*
  loadPLYAsDataSet (const char* file_name)
{
  vtkPLYReader* reader = vtkPLYReader::New ();
  reader->SetFileName (file_name);
  reader->Update ();
  return (reader->GetOutput ());
}

struct PointXYZViewpoint
{
  PCL_ADD_POINT4D;
  float vx;
  float vy;
  float vz;
};

POINT_CLOUD_REGISTER_POINT_STRUCT (PointXYZViewpoint,
                                   (float, x, x)
                                   (float, y, y)
                                   (float, z, z)
                                   (float, vx, vx)
                                   (float, vy, vy)
                                   (float, vz, vz)
);

/* ---[ */
int
  main (int argc, char** argv)
{
  if (argc < 3)
    {
    ROS_INFO("Usage %s single_view <0|1> <model.ply | model.vtk>", argv[0]);
    return -1;
    }
  std::string filename;
  // Parse the command line arguments for .vtk or .ply files
  std::vector<int> p_file_indices_vtk = terminal_tools::parse_file_extension_argument (argc, argv, ".vtk");
  std::vector<int> p_file_indices_ply = terminal_tools::parse_file_extension_argument (argc, argv, ".ply");
  bool single_view = 1;
  terminal_tools::parse_argument (argc, argv, "single_view", single_view);
  vtkSmartPointer<vtkPolyData> data;
  // Loading VTK file
  if (p_file_indices_vtk.size() != 0)
    {
     data = load_poly_data_as_data_set(argv[p_file_indices_vtk.at (0)]);
     std::stringstream filename_stream;
     filename_stream << argv[p_file_indices_vtk.at (0)];
     filename = filename_stream.str();
     ROS_INFO ("Loading vtk model with %d vertices/points.", (int)data->GetNumberOfPoints ());
    }
  //Loading PLY file
  else if (p_file_indices_ply.size() != 0)
    {
      std::stringstream filename_stream;
      filename_stream << argv[p_file_indices_ply.at (0)];
      filename = filename_stream.str();
      data = loadPLYAsDataSet (filename.c_str());
      ROS_INFO ("Loading ply model with %d vertices/points.", (int)data->GetNumberOfPoints ());
    }
  else
    {
      ROS_ERROR("Unrecognized file format");
      return -1;
    }
  // Default scan parameters
  ScanParameters sp;
  sp.nr_scans           = 900;
  sp.nr_points_in_scans = 900;
  sp.max_dist           = 30000;   // maximum distance (in mm)
  sp.vert_res           = 0.25;
  sp.hor_res            = 0.25;

  int noise_model = 0;              // set the default noise level to none
  double noise_std = 0.5;           // 0.5 standard deviations by default

  int subdiv_level = 1;
  double scan_dist = 3;
  std::string fname, base;
  char seq[256];

  // Compute start/stop for vertical and horizontal
  double vert_start = - ((double)(sp.nr_scans-1) / 2.0) * sp.vert_res;
  double vert_end   = + ((sp.nr_scans-1) * sp.vert_res) + vert_start;
  double hor_start  = - ((double)(sp.nr_points_in_scans-1) / 2.0) * sp.hor_res;
  double hor_end    = + ((sp.nr_points_in_scans-1) * sp.hor_res) + hor_start;

  // Prepare the point cloud data
  pcl::PointCloud<PointXYZViewpoint> cloud;

  // Prepare the leaves for downsampling
  pcl::VoxelGrid<PointXYZViewpoint> grid;
  grid.setLeafSize (2.5, 2.5, 2.5);    // @note: this value should be given in mm!

  // Reset and set a random seed for the Global Random Number Generator
  itpp::RNG_randomize ();
  // Create random noise distributions with mean <0> and standard deviation <std>
  itpp::Normal_RNG    n_rng   (0.0, noise_std*noise_std);
  itpp::Laplace_RNG   lap_rng (0.0, noise_std*noise_std);

  std::vector<std::string> st;
  // Virtual camera parameters
  double eye[3]     = {0.0, 0.0, 0.0};
  double viewray[3] = {0.0, 0.0, 0.0};
  double up[3]      = {0.0, 0.0, 0.0};
  double right[3]  = {0.0, 0.0, 0.0};
  double x_axis[3] = {1.0, 0.0, 0.0};
  double z_axis[3] = {0.0, 0.0, 1.0};
  double bounds[6];
  double temp_beam[3], beam[3], p[3];
  double p_coords[3], x[3], t;
  int subId;
 
  // Create a Icosahedron at center in origin and radius of 1
  vtkSmartPointer<vtkPlatonicSolidSource> icosa = vtkSmartPointer<vtkPlatonicSolidSource>::New ();
  icosa->SetSolidTypeToIcosahedron ();

  // Tesselize the source icosahedron (subdivide each triangular face
  // of the icosahedron into smaller triangles)
  vtkSmartPointer<vtkLoopSubdivisionFilter> subdivide = vtkSmartPointer<vtkLoopSubdivisionFilter>::New ();
  subdivide->SetNumberOfSubdivisions (subdiv_level);
  subdivide->SetInputConnection (icosa->GetOutputPort ());

  // Get camera positions
  vtkPolyData *sphere = subdivide->GetOutput ();
  sphere->Update ();
  if(!single_view)
    ROS_INFO ("Created %d camera position points.", (int)sphere->GetNumberOfPoints ());

    // Build a spatial locator for our dataset
    vtkSmartPointer<vtkCellLocator> tree = vtkSmartPointer<vtkCellLocator>::New ();
    tree->SetDataSet (data);
    tree->CacheCellBoundsOn ();
    tree->SetTolerance (0.0);
    tree->SetNumberOfCellsPerBucket (1);
    tree->AutomaticOn ();
    tree->BuildLocator ();
    tree->Update ();

    // Get the min-max bounds of data
    data->GetBounds (bounds);
    
    // if single view is required iterate over loop only once
    int number_of_points = sphere->GetNumberOfPoints ();
    if (single_view)
      number_of_points = 1;

    int sid = -1;
    for (int i = 0; i < number_of_points; i++)
    {
      sphere->GetPoint (i, eye);
      if (fabs(eye[0]) < EPS) eye[0] = 0;
      if (fabs(eye[1]) < EPS) eye[1] = 0;
      if (fabs(eye[2]) < EPS) eye[2] = 0;

      viewray[0] = -eye[0];
      viewray[1] = -eye[1];
      viewray[2] = -eye[2];
      eye[0] *= scan_dist;
      eye[1] *= scan_dist;
      eye[2] *= scan_dist;
      //Change here if only single view point is required
      if (single_view)
	{
	  eye[0] = 0.0;
	  eye[1] = -0.26;
	  eye[2] = -0.86;  
	  viewray[0] = 0.0;
	  viewray[1] = 0.26;
	  viewray[2] = 0.86;
	}

      if ((viewray[0] == 0) && (viewray[1] == 0))
        vtkMath::Cross (viewray, x_axis, right);
      else    
        vtkMath::Cross (viewray, z_axis, right);
      if (fabs(right[0]) < EPS) right[0] = 0;
      if (fabs(right[1]) < EPS) right[1] = 0;
      if (fabs(right[2]) < EPS) right[2] = 0;

      vtkMath::Cross (viewray, right, up);
      if (fabs(up[0]) < EPS) up[0] = 0;
      if (fabs(up[1]) < EPS) up[1] = 0;
      if (fabs(up[2]) < EPS) up[2] = 0;

      // Create a transformation
      vtkGeneralTransform* tr1 = vtkGeneralTransform::New ();
      vtkGeneralTransform* tr2 = vtkGeneralTransform::New ();
    
      // right = viewray x up
      vtkMath::Cross (viewray, up, right);
    
      // Sweep vertically
      for (double vert = vert_start; vert <= vert_end; vert += sp.vert_res)
      {
        sid++;
      
        tr1->Identity ();
        tr1->RotateWXYZ (vert, right);
        tr1->InternalTransformPoint (viewray, temp_beam);
  
        // Sweep horizontally
        int pid = -1;
        for (double hor = hor_start; hor <= hor_end; hor += sp.hor_res)
        {
          pid ++;
        
          // Create a beam vector with (lat,long) angles (vert, hor) with the viewray
          tr2->Identity ();
          tr2->RotateWXYZ (hor, up);
          tr2->InternalTransformPoint (temp_beam, beam);
          vtkMath::Normalize (beam);
  
          // Find point at max range: p = eye + beam * max_dist
          for (int d = 0; d < 3; d++)
            p[d] = eye[d] + beam[d] * sp.max_dist;
         
          // Put p_coords into laser scan at packetid = vert, scan id = hor
          vtkIdType cellId;
          if (tree->IntersectWithLine (eye, p, 0, t, x, p_coords, subId, cellId))
          {
            PointXYZViewpoint pt;
            pt.x = x[0]; pt.y = x[1]; pt.z = x[2];
            pt.vx = eye[0]; pt.vy = eye[1]; pt.vz = eye[2];
	    cloud.points.push_back(pt);
          }
        } // Horizontal
      } // Vertical
      
      // Noisify each point in the dataset
      // \note: we might decide to noisify along the ray later
      for (size_t cp = 0; cp < cloud.points.size (); ++cp)
      {
        // Add noise ?
        switch (noise_model)
        {
          // Gaussian
          case 1: { cloud.points[cp].x += n_rng (); cloud.points[cp].y += n_rng (); cloud.points[cp].z += n_rng (); break; }
          // Laplace
          case 2: { cloud.points[cp].x += lap_rng (); cloud.points[cp].y += lap_rng (); cloud.points[cp].z += lap_rng (); break; }
        }
      }

      // Downsample and remove silly po int duplicates
      pcl::PointCloud<PointXYZViewpoint> cloud_downsampled;
      grid.setInputCloud (boost::make_shared<pcl::PointCloud<PointXYZViewpoint> > (cloud));
      //grid.filter (cloud_downsampled);

      // Saves the point cloud data to disk
      sprintf (seq, "%d", (int)i);
      boost::trim (filename);
      boost::split (st, filename, boost::is_any_of ("/"), boost::token_compress_on);

      std::stringstream ss;
      std::string output_dir = st.at (st.size () - 1);
      boost::filesystem::path outpath (output_dir);
      if (!boost::filesystem::exists (outpath))
      {
        if (!boost::filesystem::create_directories (outpath))
        {
          ROS_ERROR ("Error creating directory %s.", output_dir.c_str ());
          return (-1);
        }
        ROS_INFO ("Creating directory %s", output_dir.c_str ());
      }

      fname = st.at (st.size () - 1) + seq + ".pcd";
      ROS_INFO ("Writing %d points to %s", (int)cloud.points.size (), fname.c_str ());
      pcl::io::savePCDFile (fname.c_str (), cloud);
    } // sphere 
  return (0);
}
/* ]--- */