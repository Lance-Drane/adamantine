/* Copyright (c) 2023, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#ifndef POINT_CLOUD_HH
#define POINT_CLOUD_HH

#include <ExperimentalData.hh>

#include <deal.II/dofs/dof_handler.h>

namespace adamantine
{
/**
 * Point cloud with the associated temperature.
 */
template <int dim>
class PointCloud final : public ExperimentalData<dim>
{
public:
  /**
   * Constructor.
   */
  PointCloud(boost::property_tree::ptree const &experiment_database);

  unsigned int read_next_frame() override;

  PointsValues<dim> get_points_values() override;

private:
  /**
   * Next frame that should be read.
   */
  unsigned int _next_frame;
  /**
   * ID of the first camera.
   */
  unsigned int _first_camera_id;
  /**
   * ID of the last camera.
   */
  unsigned int _last_camera_id;
  /**
   * Generic file name of the frames.
   */
  std::string _data_filename;
  /**
   * Values and associated points of the current frame.
   */
  PointsValues<dim> _points_values_current_frame;
};

} // namespace adamantine

#endif
