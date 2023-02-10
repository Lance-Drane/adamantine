/* Copyright (c) 2021, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#include <deal.II/base/mpi.h>
#define BOOST_TEST_MODULE ExperimentaData

#include <Geometry.hh>
#include <experimental_data.hh>

#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_nothing.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/grid/filtered_iterator.h>

#include "main.cc"

namespace utf = boost::unit_test;

BOOST_AUTO_TEST_CASE(read_experimental_data_point_cloud_from_file)
{
  MPI_Comm communicator = MPI_COMM_WORLD;

  boost::property_tree::ptree database;
  database.put("file", "experimental_data_#camera_#frame.csv");
  database.put("last_frame", 0);
  database.put("first_camera_id", 0);
  database.put("last_camera_id", 0);
  database.put("data_columns", "1,2,3,5");

  auto points_values =
      adamantine::read_experimental_data_point_cloud<3>(communicator, database);

  std::vector<double> values_ref = {1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.};
  std::vector<dealii::Point<3>> points_ref;
  points_ref.emplace_back(0, 0, 1.);
  points_ref.emplace_back(0, 0.5, 1.);
  points_ref.emplace_back(0, 1., 1.);
  points_ref.emplace_back(0.5, 0., 1.);
  points_ref.emplace_back(0.5, 0.5, 1.);
  points_ref.emplace_back(0.5, 1., 1.);
  points_ref.emplace_back(1., 0., 1.);
  points_ref.emplace_back(1., 0.5, 1.);
  points_ref.emplace_back(1., 1., 1.);

  BOOST_TEST(points_values.size() == 1);
  BOOST_TEST(points_values[0].points.size() == 9);
  BOOST_TEST(points_values[0].points.size() == points_values[0].values.size());
  for (unsigned int i = 0; i < points_values[0].points.size(); ++i)
  {
    BOOST_TEST(points_values[0].values[i] == values_ref[i]);
    BOOST_TEST(points_values[0].points[i] == points_ref[i]);
  }
}

BOOST_AUTO_TEST_CASE(set_vector_with_experimental_data_point_cloud)
{
  MPI_Comm communicator = MPI_COMM_WORLD;
  auto n_mpi_processes = dealii::Utilities::MPI::n_mpi_processes(communicator);

  adamantine::PointsValues<3> points_values;
  // Create the points
  points_values.points.emplace_back(0, 0, 1.);
  points_values.points.emplace_back(0, 0.5, 1.);
  points_values.points.emplace_back(0, 1., 1.);
  points_values.points.emplace_back(0.5, 0., 1.);
  points_values.points.emplace_back(0.5, 0.5, 1.);
  points_values.points.emplace_back(0.5, 1., 1.);
  points_values.points.emplace_back(1., 0., 1.);
  points_values.points.emplace_back(1., 0.5, 1.);
  points_values.points.emplace_back(1., 1., 1.);
  // Create the values
  points_values.values = {1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.};

  boost::property_tree::ptree database;
  database.put("import_mesh", false);
  database.put("length", 1);
  database.put("length_divisions", 2);
  database.put("height", 1);
  database.put("height_divisions", 2);
  database.put("width", 1);
  database.put("width_divisions", 2);
  adamantine::Geometry<3> geometry(communicator, database);
  dealii::parallel::distributed::Triangulation<3> const &tria =
      geometry.get_triangulation();

  dealii::FE_Q<3> fe(1);
  dealii::DoFHandler<3> dof_handler(tria);
  dof_handler.distribute_dofs(fe);

  auto locally_owned_dofs = dof_handler.locally_owned_dofs();
  dealii::IndexSet locally_relevant_dofs;
  dealii::DoFTools::extract_locally_relevant_dofs(dof_handler,
                                                  locally_relevant_dofs);
  dealii::LinearAlgebra::distributed::Vector<double> temperature(
      locally_owned_dofs, locally_relevant_dofs, communicator);

  auto [dof_indices, indices_and_offsets] =
      adamantine::get_indices_and_offsets(points_values, dof_handler);

  // This used to be in set_with_experimental_data
  unsigned int const n_queries = points_values.points.size();
  for (unsigned int i = 0; i < n_queries; ++i)
  {
    for (int j = indices_and_offsets.second[i];
         j < indices_and_offsets.second[i + 1]; ++j)
    {
      temperature[dof_indices[indices_and_offsets.first[j]]] =
          points_values.values[i];
    }
  }

  temperature.compress(dealii::VectorOperation::insert);

  std::vector<double> temperature_ref = {
      0, 0, 0, 0, 0,   0,   0,   0,   0,   0,   0,   0,   0, 0,
      0, 0, 0, 0, 1.2, 1.5, 1.3, 1.6, 1.8, 1.9, 1.4, 1.7, 2};

  for (unsigned int i = 0; i < temperature.locally_owned_size(); ++i)
  {
    BOOST_TEST(temperature.local_element(i) ==
               temperature_ref[locally_owned_dofs.nth_index_in_set(i)]);
  }
}

BOOST_AUTO_TEST_CASE(read_experimental_data_ray_tracing_from_file)
{
  MPI_Comm communicator = MPI_COMM_WORLD;

  boost::property_tree::ptree database;
  database.put("import_mesh", false);
  database.put("length", 1);
  database.put("length_divisions", 2);
  database.put("height", 1);
  database.put("height_divisions", 2);
  database.put("width", 1);
  database.put("width_divisions", 2);
  adamantine::Geometry<3> geometry(communicator, database);
  dealii::parallel::distributed::Triangulation<3> const &tria =
      geometry.get_triangulation();

  dealii::FE_Q<3> fe(1);
  dealii::DoFHandler<3> dof_handler(tria);
  dof_handler.distribute_dofs(fe);

  // Read the rays from file
  boost::property_tree::ptree experiment_database;
  experiment_database.put("file",
                          "raytracing_experimental_data_#camera_#frame.csv");
  experiment_database.put("last_frame", 0);
  experiment_database.put("first_camera_id", 0);
  experiment_database.put("last_camera_id", 0);
  adamantine::RayTracing ray_tracing(experiment_database);

  // Compute the intersection points
  unsigned int frame = 0;
  auto points_values = ray_tracing.get_intersection(dof_handler, frame);

  // Reference solution
  std::vector<double> values_ref = {1, 2, 3, 5};
  std::vector<dealii::Point<3>> points_ref;
  points_ref.emplace_back(0., 0.1, 0.2);
  points_ref.emplace_back(1., 0.1, 0.001);
  points_ref.emplace_back(1., 0.5, 0.001);
  points_ref.emplace_back(1., 0.5, 0.4999);

  if (dealii::Utilities::MPI::this_mpi_process(communicator) == 0)
  {
    BOOST_TEST(points_values.values.size() = values_ref.size());
    BOOST_TEST(points_values.points.size() = points_ref.size());
    for (unsigned int i = 0; i < values_ref.size(); ++i)
    {
      BOOST_TEST(points_values.values[i] == values_ref[i]);
      BOOST_TEST(points_values.points[i] == points_ref[i]);
    }
  }
}

BOOST_AUTO_TEST_CASE(timestamp, *utf::tolerance(1e-12))
{
  boost::property_tree::ptree database;
  database.put("log_filename", "experiment_log_test.txt");
  database.put("first_frame_temporal_offset", 0.1);
  database.put("first_frame", 1);
  database.put("last_frame", 3);
  database.put("first_camera_id", 0);
  database.put("last_camera_id", 1);

  std::vector<std::vector<double>> time_stamps =
      adamantine::read_frame_timestamps(database);

  BOOST_TEST(time_stamps.size() == 2);
  BOOST_TEST(time_stamps[0].size() == 3);
  BOOST_TEST(time_stamps[1].size() == 3);

  BOOST_TEST(time_stamps[0][0] == 0.1);
  BOOST_TEST(time_stamps[0][1] == 0.1135);
  BOOST_TEST(time_stamps[0][2] == 0.1345);

  BOOST_TEST(time_stamps[1][0] == 0.1);
  BOOST_TEST(time_stamps[1][1] == 0.1136);
  BOOST_TEST(time_stamps[1][2] == 0.1348);
}

BOOST_AUTO_TEST_CASE(project_ray_data_on_mesh, *utf::tolerance(1e-12))
{
  // NOTE: Currently this is using an IR data file that's not calibrated
  // particularly well. That's ok for these purposes, but we may eventually
  // want to switch to a "better" IR file.

  MPI_Comm communicator = MPI_COMM_WORLD;
  auto n_mpi_processes = dealii::Utilities::MPI::n_mpi_processes(communicator);
  if (n_mpi_processes == 1)
  {

    // Oversize version of the mesh from the Tormach wall build
    boost::property_tree::ptree database;
    database.put("import_mesh", false);
    database.put("length", 400.0e-3);
    database.put("length_divisions", 8);
    database.put("height", 200.0e-3);
    database.put("height_divisions", 4);
    database.put("width", 400.0e-3);
    database.put("width_divisions", 8);
    database.put("material_height", 100.0e-3);

    adamantine::Geometry<3> geometry(communicator, database);
    dealii::parallel::distributed::Triangulation<3> const &tria =
        geometry.get_triangulation();

    dealii::hp::FECollection<3> fe_collection;
    fe_collection.push_back(dealii::FE_Q<3>(1));
    fe_collection.push_back(dealii::FE_Nothing<3>());
    dealii::DoFHandler<3> dof_handler(tria);
    dof_handler.distribute_dofs(fe_collection);

    auto active_cells = 0;
    auto inactive_cells = 0;

    double const material_height = database.get("material_height", 1e9);
    for (auto const &cell :
         dealii::filter_iterators(dof_handler.active_cell_iterators(),
                                  dealii::IteratorFilters::LocallyOwnedCell()))
    {
      // If the center of the cell is below material_height, it contains
      // material otherwise it does not.
      if (cell->center()[2] < material_height)
      {
        cell->set_active_fe_index(0);
        active_cells++;
      }
      else
      {
        cell->set_active_fe_index(1);
        inactive_cells++;
      }
    }

    BOOST_CHECK(active_cells == 128);
    BOOST_CHECK(inactive_cells == 128);

    // Read the rays from file
    boost::property_tree::ptree experiment_database;
    experiment_database.put("file", "rays_cam-#camera-#frame_test_full.csv");
    experiment_database.put("last_frame", 0);
    experiment_database.put("first_camera_id", 0);
    experiment_database.put("last_camera_id", 0);

    adamantine::RayTracing ray_tracing(experiment_database);

    // Compute the intersection points
    unsigned int frame = 0;
    auto points_values = ray_tracing.get_intersection(dof_handler, frame);

    BOOST_CHECK(points_values.points.size() == 58938);

    // Get the indices and offsets
    auto [dof_indices, indices_and_offsets] =
        adamantine::get_indices_and_offsets<3>(points_values, dof_handler);

    BOOST_CHECK(indices_and_offsets.first.size() == 58938);
  }
  else
  {
    std::cout << "'project_ray_data_on_mesh' is currently skipped for multiple "
                 "MPI processes"
              << std::endl;
  }
}
